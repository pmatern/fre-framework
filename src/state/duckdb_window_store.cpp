#ifdef FRE_ENABLE_DUCKDB

#include <fre/state/duckdb_window_store.hpp>

#include <duckdb.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <mutex>
#include <thread>

namespace fre {

// ─── RAII wrappers around DuckDB C API handles ───────────────────────────────

namespace {

struct DbHandle {
    duckdb_database db{nullptr};
    DbHandle() = default;
    DbHandle(const DbHandle&) = delete;
    DbHandle& operator=(const DbHandle&) = delete;
    DbHandle(DbHandle&& o) noexcept : db{o.db} { o.db = nullptr; }
    DbHandle& operator=(DbHandle&& o) noexcept {
        if (db) duckdb_close(&db);
        db = o.db; o.db = nullptr; return *this;
    }
    ~DbHandle() { if (db) duckdb_close(&db); }
};

struct ConnHandle {
    duckdb_connection conn{nullptr};
    ConnHandle() = default;
    ConnHandle(const ConnHandle&) = delete;
    ConnHandle& operator=(const ConnHandle&) = delete;
    ConnHandle(ConnHandle&& o) noexcept : conn{o.conn} { o.conn = nullptr; }
    ConnHandle& operator=(ConnHandle&& o) noexcept {
        if (conn) duckdb_disconnect(&conn);
        conn = o.conn; o.conn = nullptr; return *this;
    }
    ~ConnHandle() { if (conn) duckdb_disconnect(&conn); }
};

struct StmtHandle {
    duckdb_prepared_statement stmt{nullptr};
    StmtHandle() = default;
    StmtHandle(const StmtHandle&) = delete;
    StmtHandle& operator=(const StmtHandle&) = delete;
    ~StmtHandle() { if (stmt) duckdb_destroy_prepare(&stmt); }
};

struct ResultHandle {
    duckdb_result result{};
    bool          valid{false};
    ResultHandle() = default;
    ResultHandle(const ResultHandle&) = delete;
    ResultHandle& operator=(const ResultHandle&) = delete;
    ~ResultHandle() { if (valid) duckdb_destroy_result(&result); }
};

// Helper: execute a non-parameterised SQL string; returns error string or empty.
std::string exec_sql(duckdb_connection conn, const char* sql) {
    ResultHandle rh;
    if (duckdb_query(conn, sql, &rh.result) == DuckDBError) {
        rh.valid = true;
        return duckdb_result_error(&rh.result);
    }
    rh.valid = true;
    return {};
}

}  // namespace

// ─── DuckDbWindowStore::Impl ─────────────────────────────────────────────────

struct DuckDbWindowStore::Impl {
    DuckDbConfig       config;
    DbHandle           db;
    ConnHandle         conn;      // hot-path: protected by mutex_
    ConnHandle         query_conn; // query_range: protected by query_mutex_
    std::mutex         mutex_;
    std::mutex         query_mutex_;
    std::atomic<bool>  available_{false};
    std::jthread       flush_thread_;

    explicit Impl(DuckDbConfig cfg) : config{std::move(cfg)} {
        const char* path = config.db_path.empty() ? nullptr : config.db_path.c_str();
        if (duckdb_open(path, &db.db) == DuckDBError) return;
        if (duckdb_connect(db.db, &conn.conn) == DuckDBError) return;
        if (duckdb_connect(db.db, &query_conn.conn) == DuckDBError) return;

        // DuckDB 1.4+ moved aggregate/scalar functions to the core_functions extension.
        // Enable auto-loading so SUM, COALESCE, etc. are available without explicit LOAD.
        // These are built-in extensions (no network needed); autoinstall covers fresh envs.
        const char* ext_sql =
            "SET autoinstall_known_extensions=1;"
            "SET autoload_known_extensions=1";
        if (!exec_sql(conn.conn, ext_sql).empty()) return;
        if (!exec_sql(query_conn.conn, ext_sql).empty()) return;

        // Create schema
        // Note: no updated_at column — now() moved to core_functions ext in DuckDB 1.4+.
        const std::string ddl =
            "CREATE TABLE IF NOT EXISTS window_state ("
            "  tenant_id   VARCHAR  NOT NULL,"
            "  entity_id   VARCHAR  NOT NULL,"
            "  window_name VARCHAR  NOT NULL,"
            "  epoch       UBIGINT  NOT NULL,"
            "  aggregate   DOUBLE   NOT NULL DEFAULT 0.0,"
            "  version     UBIGINT  NOT NULL DEFAULT 0,"
            "  PRIMARY KEY (tenant_id, entity_id, window_name, epoch)"
            ")";
        const auto err = exec_sql(conn.conn, ddl.c_str());
        if (!err.empty()) return;

        available_.store(true, std::memory_order_release);

        // Start background flush thread if archival is configured
        if (!config.parquet_archive_dir.empty() && config.flush_interval_ms > 0) {
            flush_thread_ = std::jthread([this](std::stop_token stop) {
                run_flush_loop(stop);
            });
        }
    }

    void run_flush_loop(std::stop_token stop) {
        using namespace std::chrono_literals;
        const auto interval = std::chrono::milliseconds{config.flush_interval_ms};

        while (!stop.stop_requested()) {
            std::this_thread::sleep_for(interval);
            if (stop.stop_requested()) break;
            flush_old_epochs();
        }
    }

    void flush_old_epochs() {
        if (!available_.load(std::memory_order_acquire)) return;

        const uint64_t now_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
            .count());

        const uint64_t current_epoch = (config.window_ms > 0)
                                           ? (now_ms / config.window_ms)
                                           : 0;
        if (current_epoch < config.warm_epoch_retention) return;

        const uint64_t cutoff_epoch = current_epoch - config.warm_epoch_retention;

        // Export each old epoch to parquet, then delete from warm tier.
        // We query distinct old epochs first, then process each one.
        std::string query_epochs_sql = std::format(
            "SELECT DISTINCT epoch FROM window_state WHERE epoch < {}", cutoff_epoch);

        ConnHandle flush_conn;
        {
            std::lock_guard<std::mutex> lock{mutex_};
            if (duckdb_connect(db.db, &flush_conn.conn) != DuckDBSuccess) return;
        }

        ResultHandle epochs_result;
        if (duckdb_query(flush_conn.conn, query_epochs_sql.c_str(), &epochs_result.result)
                == DuckDBError) {
            epochs_result.valid = true;
            return;
        }
        epochs_result.valid = true;

        const idx_t row_count = duckdb_row_count(&epochs_result.result);
        for (idx_t row = 0; row < row_count; ++row) {
            const uint64_t epoch_val = duckdb_value_uint64(&epochs_result.result, 0, row);

            // Create the partition directory
            const std::string epoch_dir = std::format(
                "{}/epoch={}", config.parquet_archive_dir, epoch_val);
            std::filesystem::create_directories(epoch_dir);

            const std::string parquet_path = epoch_dir + "/part-0000.parquet";
            const std::string copy_sql = std::format(
                "COPY (SELECT tenant_id, entity_id, window_name, epoch, aggregate, version"
                "      FROM window_state WHERE epoch = {})"
                " TO '{}' (FORMAT PARQUET)",
                epoch_val, parquet_path);

            exec_sql(flush_conn.conn, copy_sql.c_str());

            const std::string delete_sql = std::format(
                "DELETE FROM window_state WHERE epoch = {}", epoch_val);
            exec_sql(flush_conn.conn, delete_sql.c_str());
        }
    }
};

// ─── DuckDbWindowStore public API ────────────────────────────────────────────

DuckDbWindowStore::DuckDbWindowStore(DuckDbConfig config)
    : impl_{std::make_unique<Impl>(std::move(config))}
{}

DuckDbWindowStore::~DuckDbWindowStore() = default;

DuckDbWindowStore::DuckDbWindowStore(DuckDbWindowStore&&) noexcept = default;
DuckDbWindowStore& DuckDbWindowStore::operator=(DuckDbWindowStore&&) noexcept = default;

bool DuckDbWindowStore::is_available() const noexcept {
    return impl_->available_.load(std::memory_order_acquire);
}

std::expected<WindowValue, StoreError> DuckDbWindowStore::get(const WindowKey& key) {
    if (!is_available()) {
        return std::unexpected(StoreError{StoreErrorCode::Unavailable, "DuckDB unavailable"});
    }

    std::lock_guard<std::mutex> lock{impl_->mutex_};

    StmtHandle sh;
    if (duckdb_prepare(impl_->conn.conn,
            "SELECT aggregate, version FROM window_state"
            " WHERE tenant_id=$1 AND entity_id=$2 AND window_name=$3 AND epoch=$4",
            &sh.stmt) == DuckDBError) {
        return std::unexpected(StoreError{StoreErrorCode::Unavailable, "prepare failed"});
    }

    duckdb_bind_varchar(sh.stmt, 1, key.tenant_id.c_str());
    duckdb_bind_varchar(sh.stmt, 2, key.entity_id.c_str());
    duckdb_bind_varchar(sh.stmt, 3, key.window_name.c_str());
    duckdb_bind_uint64 (sh.stmt, 4, key.epoch);

    ResultHandle rh;
    if (duckdb_execute_prepared(sh.stmt, &rh.result) == DuckDBError) {
        rh.valid = true;
        return std::unexpected(StoreError{
            StoreErrorCode::Unavailable, duckdb_result_error(&rh.result)});
    }
    rh.valid = true;

    if (duckdb_row_count(&rh.result) == 0) {
        return WindowValue{.aggregate = 0.0, .version = 0};
    }

    return WindowValue{
        .aggregate = duckdb_value_double(&rh.result, 0, 0),
        .version   = duckdb_value_uint64(&rh.result, 1, 0),
    };
}

std::expected<bool, StoreError> DuckDbWindowStore::compare_and_swap(
    const WindowKey&   key,
    const WindowValue& expected_val,
    const WindowValue& new_val)
{
    if (!is_available()) {
        return std::unexpected(StoreError{StoreErrorCode::Unavailable, "DuckDB unavailable"});
    }

    std::lock_guard<std::mutex> lock{impl_->mutex_};

    // Begin transaction for atomic seed + update.
    ResultHandle begin_rh;
    if (duckdb_query(impl_->conn.conn, "BEGIN", &begin_rh.result) == DuckDBError) {
        begin_rh.valid = true;
        return std::unexpected(StoreError{StoreErrorCode::Unavailable, "BEGIN failed"});
    }
    begin_rh.valid = true;

    // Step 1: Seed the row with default values if it doesn't exist yet.
    StmtHandle insert_sh;
    if (duckdb_prepare(impl_->conn.conn,
            "INSERT INTO window_state (tenant_id, entity_id, window_name, epoch,"
            " aggregate, version)"
            " VALUES ($1, $2, $3, $4, 0.0, 0)"
            " ON CONFLICT DO NOTHING",
            &insert_sh.stmt) == DuckDBError)
    {
        duckdb_query(impl_->conn.conn, "ROLLBACK", nullptr);
        return std::unexpected(StoreError{StoreErrorCode::Unavailable, "prepare insert failed"});
    }
    duckdb_bind_varchar(insert_sh.stmt, 1, key.tenant_id.c_str());
    duckdb_bind_varchar(insert_sh.stmt, 2, key.entity_id.c_str());
    duckdb_bind_varchar(insert_sh.stmt, 3, key.window_name.c_str());
    duckdb_bind_uint64 (insert_sh.stmt, 4, key.epoch);

    ResultHandle ins_rh;
    if (duckdb_execute_prepared(insert_sh.stmt, &ins_rh.result) == DuckDBError) {
        ins_rh.valid = true;
        duckdb_query(impl_->conn.conn, "ROLLBACK", nullptr);
        return std::unexpected(StoreError{StoreErrorCode::Unavailable,
                                          duckdb_result_error(&ins_rh.result)});
    }
    ins_rh.valid = true;

    // Step 2: Conditional UPDATE — only if version matches.
    StmtHandle upd_sh;
    if (duckdb_prepare(impl_->conn.conn,
            "UPDATE window_state"
            " SET aggregate=$1, version=$2"
            " WHERE tenant_id=$3 AND entity_id=$4 AND window_name=$5"
            "   AND epoch=$6 AND version=$7",
            &upd_sh.stmt) == DuckDBError)
    {
        duckdb_query(impl_->conn.conn, "ROLLBACK", nullptr);
        return std::unexpected(StoreError{StoreErrorCode::Unavailable, "prepare update failed"});
    }
    duckdb_bind_double (upd_sh.stmt, 1, new_val.aggregate);
    duckdb_bind_uint64 (upd_sh.stmt, 2, new_val.version);
    duckdb_bind_varchar(upd_sh.stmt, 3, key.tenant_id.c_str());
    duckdb_bind_varchar(upd_sh.stmt, 4, key.entity_id.c_str());
    duckdb_bind_varchar(upd_sh.stmt, 5, key.window_name.c_str());
    duckdb_bind_uint64 (upd_sh.stmt, 6, key.epoch);
    duckdb_bind_uint64 (upd_sh.stmt, 7, expected_val.version);

    ResultHandle upd_rh;
    if (duckdb_execute_prepared(upd_sh.stmt, &upd_rh.result) == DuckDBError) {
        upd_rh.valid = true;
        duckdb_query(impl_->conn.conn, "ROLLBACK", nullptr);
        return std::unexpected(StoreError{StoreErrorCode::Unavailable,
                                          duckdb_result_error(&upd_rh.result)});
    }
    upd_rh.valid = true;

    const idx_t rows_changed = duckdb_rows_changed(&upd_rh.result);

    duckdb_query(impl_->conn.conn, "COMMIT", nullptr);

    return rows_changed > 0;
}

std::expected<void, StoreError> DuckDbWindowStore::expire(const WindowKey& key) {
    if (!is_available()) {
        return std::unexpected(StoreError{StoreErrorCode::Unavailable, "DuckDB unavailable"});
    }

    std::lock_guard<std::mutex> lock{impl_->mutex_};

    StmtHandle sh;
    if (duckdb_prepare(impl_->conn.conn,
            "DELETE FROM window_state"
            " WHERE tenant_id=$1 AND entity_id=$2 AND window_name=$3 AND epoch=$4",
            &sh.stmt) == DuckDBError) {
        return std::unexpected(StoreError{StoreErrorCode::Unavailable, "prepare failed"});
    }
    duckdb_bind_varchar(sh.stmt, 1, key.tenant_id.c_str());
    duckdb_bind_varchar(sh.stmt, 2, key.entity_id.c_str());
    duckdb_bind_varchar(sh.stmt, 3, key.window_name.c_str());
    duckdb_bind_uint64 (sh.stmt, 4, key.epoch);

    ResultHandle rh;
    if (duckdb_execute_prepared(sh.stmt, &rh.result) == DuckDBError) {
        rh.valid = true;
        return std::unexpected(StoreError{
            StoreErrorCode::Unavailable, duckdb_result_error(&rh.result)});
    }
    rh.valid = true;
    return {};
}

// ─── Helper: read varchar column from result row ──────────────────────────────

namespace {
std::string read_varchar(duckdb_result& r, idx_t col, idx_t row) {
    char* c = duckdb_value_varchar(&r, col, row);
    if (!c) return {};
    std::string s{c};
    duckdb_free(c);
    return s;
}
}  // namespace

// ─── upsert_batch ─────────────────────────────────────────────────────────────

std::expected<void, StoreError>
DuckDbWindowStore::upsert_batch(std::span<const std::pair<WindowKey, WindowValue>> entries)
{
    if (!is_available()) {
        return std::unexpected(StoreError{StoreErrorCode::Unavailable, "DuckDB unavailable"});
    }
    if (entries.empty()) return {};

    std::lock_guard<std::mutex> lock{impl_->mutex_};

    ResultHandle begin_rh;
    if (duckdb_query(impl_->conn.conn, "BEGIN", &begin_rh.result) == DuckDBError) {
        begin_rh.valid = true;
        return std::unexpected(StoreError{StoreErrorCode::Unavailable, "upsert_batch BEGIN failed"});
    }
    begin_rh.valid = true;

    for (const auto& [key, val] : entries) {
        StmtHandle sh;
        if (duckdb_prepare(impl_->conn.conn,
                "INSERT INTO window_state"
                " (tenant_id, entity_id, window_name, epoch, aggregate, version)"
                " VALUES ($1, $2, $3, $4, $5, $6)"
                " ON CONFLICT (tenant_id, entity_id, window_name, epoch)"
                " DO UPDATE SET aggregate=EXCLUDED.aggregate,"
                "               version=EXCLUDED.version",
                &sh.stmt) == DuckDBError)
        {
            duckdb_query(impl_->conn.conn, "ROLLBACK", nullptr);
            return std::unexpected(StoreError{StoreErrorCode::Unavailable, "upsert prepare failed"});
        }
        duckdb_bind_varchar(sh.stmt, 1, key.tenant_id.c_str());
        duckdb_bind_varchar(sh.stmt, 2, key.entity_id.c_str());
        duckdb_bind_varchar(sh.stmt, 3, key.window_name.c_str());
        duckdb_bind_uint64 (sh.stmt, 4, key.epoch);
        duckdb_bind_double (sh.stmt, 5, val.aggregate);
        duckdb_bind_uint64 (sh.stmt, 6, val.version);

        ResultHandle rh;
        if (duckdb_execute_prepared(sh.stmt, &rh.result) == DuckDBError) {
            rh.valid = true;
            duckdb_query(impl_->conn.conn, "ROLLBACK", nullptr);
            return std::unexpected(StoreError{
                StoreErrorCode::Unavailable, duckdb_result_error(&rh.result)});
        }
        rh.valid = true;
    }

    ResultHandle commit_rh;
    if (duckdb_query(impl_->conn.conn, "COMMIT", &commit_rh.result) == DuckDBError) {
        commit_rh.valid = true;
        duckdb_query(impl_->conn.conn, "ROLLBACK", nullptr);
        return std::unexpected(StoreError{StoreErrorCode::Unavailable, "upsert_batch COMMIT failed"});
    }
    commit_rh.valid = true;
    return {};
}

// ─── scan_warm_tier ───────────────────────────────────────────────────────────

std::expected<std::vector<std::pair<WindowKey, WindowValue>>, StoreError>
DuckDbWindowStore::scan_warm_tier()
{
    if (!is_available()) {
        return std::unexpected(StoreError{StoreErrorCode::Unavailable, "DuckDB unavailable"});
    }

    std::lock_guard<std::mutex> lock{impl_->query_mutex_};

    ResultHandle rh;
    if (duckdb_query(impl_->query_conn.conn,
            "SELECT tenant_id, entity_id, window_name, epoch, aggregate, version"
            " FROM window_state",
            &rh.result) == DuckDBError) {
        rh.valid = true;
        return std::unexpected(StoreError{
            StoreErrorCode::QueryRangeError, duckdb_result_error(&rh.result)});
    }
    rh.valid = true;

    std::vector<std::pair<WindowKey, WindowValue>> out;
    const idx_t row_count = duckdb_row_count(&rh.result);
    out.reserve(static_cast<std::size_t>(row_count));

    for (idx_t row = 0; row < row_count; ++row) {
        WindowKey key;
        key.tenant_id   = read_varchar(rh.result, 0, row);
        key.entity_id   = read_varchar(rh.result, 1, row);
        key.window_name = read_varchar(rh.result, 2, row);
        key.epoch       = duckdb_value_uint64(&rh.result, 3, row);

        WindowValue val;
        val.aggregate = duckdb_value_double(&rh.result, 4, row);
        val.version   = duckdb_value_uint64(&rh.result, 5, row);

        out.emplace_back(std::move(key), val);
    }

    return out;
}

// ─── as_backend ───────────────────────────────────────────────────────────────

ExternalStoreBackend DuckDbWindowStore::as_backend() {
    return ExternalStoreBackend{
        .get             = [this](const WindowKey& k)              { return get(k); },
        .compare_and_swap = [this](const WindowKey& k, const WindowValue& e, const WindowValue& n) {
            return compare_and_swap(k, e, n);
        },
        .expire          = [this](const WindowKey& k)              { return expire(k); },
        .is_available    = [this]()                                { return is_available(); },
    };
}

std::expected<double, StoreError> DuckDbWindowStore::query_range(
    std::string_view tenant_id,
    std::string_view entity_id,
    std::string_view window_name,
    uint64_t         epoch_start,
    uint64_t         epoch_end)
{
    if (!is_available()) {
        return std::unexpected(StoreError{StoreErrorCode::Unavailable, "DuckDB unavailable"});
    }

    std::lock_guard<std::mutex> lock{impl_->query_mutex_};

    // Query warm tier
    StmtHandle sh;
    const char* warm_sql =
        "SELECT COALESCE(SUM(aggregate), 0.0) FROM window_state"
        " WHERE tenant_id=$1 AND entity_id=$2 AND window_name=$3"
        "   AND epoch >= $4 AND epoch <= $5";

    if (duckdb_prepare(impl_->query_conn.conn, warm_sql, &sh.stmt) == DuckDBError) {
        return std::unexpected(StoreError{StoreErrorCode::QueryRangeError, "prepare failed"});
    }
    duckdb_bind_varchar(sh.stmt, 1, std::string{tenant_id}.c_str());
    duckdb_bind_varchar(sh.stmt, 2, std::string{entity_id}.c_str());
    duckdb_bind_varchar(sh.stmt, 3, std::string{window_name}.c_str());
    duckdb_bind_uint64 (sh.stmt, 4, epoch_start);
    duckdb_bind_uint64 (sh.stmt, 5, epoch_end);

    ResultHandle rh;
    if (duckdb_execute_prepared(sh.stmt, &rh.result) == DuckDBError) {
        rh.valid = true;
        return std::unexpected(StoreError{
            StoreErrorCode::QueryRangeError, duckdb_result_error(&rh.result)});
    }
    rh.valid = true;

    double warm_sum = (duckdb_row_count(&rh.result) > 0)
                          ? duckdb_value_double(&rh.result, 0, 0)
                          : 0.0;

    // Query cold tier (parquet archive) if configured
    double cold_sum = 0.0;
    if (!impl_->config.parquet_archive_dir.empty()) {
        const std::string glob =
            impl_->config.parquet_archive_dir + "/epoch=*/part-*.parquet";

        // Check if any parquet files exist before querying
        std::error_code fs_ec;
        bool has_parquet = false;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                 impl_->config.parquet_archive_dir, fs_ec)) {
            if (entry.path().extension() == ".parquet") {
                has_parquet = true;
                break;
            }
        }
        (void)fs_ec; // ignore iteration errors (dir may not exist yet)

        if (has_parquet) {
            const std::string cold_sql = std::format(
                "SELECT COALESCE(SUM(aggregate), 0.0)"
                " FROM read_parquet('{}')"
                " WHERE tenant_id='{}' AND entity_id='{}' AND window_name='{}'"
                "   AND epoch >= {} AND epoch <= {}",
                glob, tenant_id, entity_id, window_name, epoch_start, epoch_end);

            ResultHandle cold_rh;
            if (duckdb_query(impl_->query_conn.conn, cold_sql.c_str(), &cold_rh.result)
                    == DuckDBSuccess) {
                cold_rh.valid = true;
                if (duckdb_row_count(&cold_rh.result) > 0) {
                    cold_sum = duckdb_value_double(&cold_rh.result, 0, 0);
                }
            } else {
                cold_rh.valid = true;
            }
        }
    }

    return warm_sum + cold_sum;
}

}  // namespace fre

#endif  // FRE_ENABLE_DUCKDB
