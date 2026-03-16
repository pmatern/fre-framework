/// ServiceHarness implementation — separated from main() so that tests can link
/// against the library without pulling in the entry point.

#include <fre/service/harness.hpp>
#include <fre/core/logging.hpp>
#include <fre/pipeline/pipeline_config.hpp>

#include <asio/connect.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/read.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include <nlohmann/json.hpp>

#include <atomic>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

namespace fre::service {

using json = nlohmann::json;

// ─── ServiceHarness::Impl ────────────────────────────────────────────────────

struct ServiceHarness::Impl {
    HarnessConfig              config;
    fre::Pipeline              pipeline;
    asio::io_context           ioc;
    asio::ip::tcp::acceptor    acceptor{ioc};
    std::thread                io_thread;
    std::atomic<bool>          running{false};

    explicit Impl(HarnessConfig cfg)
        : config{std::move(cfg)}
        , pipeline{[&]() -> fre::PipelineConfig {
            if (!config.pipeline_config_path.empty()) {
                auto maybe = load_config_from_json(config.pipeline_config_path);
                if (maybe) return std::move(*maybe);
            }
            return std::move(config.pipeline_config);
          }()}
    {}

    static void handle_connection(asio::ip::tcp::socket sock, Impl& impl);
};

// ─── HTTP helpers ────────────────────────────────────────────────────────────

namespace {

struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
    int         content_length{0};
};

std::optional<HttpRequest> parse_request(const std::string& raw) {
    HttpRequest req;
    std::istringstream stream{raw};
    std::string line;

    if (!std::getline(stream, line)) return std::nullopt;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    {
        std::istringstream rl{line};
        std::string version;
        rl >> req.method >> req.path >> version;
    }

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        const auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name  = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            while (!value.empty() && value.front() == ' ') value.erase(0, 1);
            for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (name == "content-length") {
                req.content_length = std::stoi(value);
            }
        }
    }

    if (req.content_length > 0) {
        req.body.resize(static_cast<std::size_t>(req.content_length));
        stream.read(req.body.data(), req.content_length);
    }

    return req;
}

std::string http_response(int status, const std::string& reason,
                           const std::string& content_type,
                           const std::string& body)
{
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status << " " << reason << "\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << body;
    return oss.str();
}

}  // namespace (http helpers)

void ServiceHarness::Impl::handle_connection(asio::ip::tcp::socket sock, Impl& impl) {
    try {
        asio::streambuf buf;
        asio::error_code ec;

        std::size_t n = asio::read_until(sock, buf, "\r\n\r\n", ec);
        if (ec) return;

        std::string raw_headers{
            asio::buffers_begin(buf.data()),
            asio::buffers_begin(buf.data()) + static_cast<std::ptrdiff_t>(n)};
        buf.consume(n);

        auto maybe_req = parse_request(raw_headers);
        if (!maybe_req) {
            asio::write(sock, asio::buffer(
                http_response(400, "Bad Request", "text/plain", "bad request")), ec);
            return;
        }
        auto& req = *maybe_req;

        if (req.content_length > 0) {
            std::size_t already = buf.size();
            std::size_t needed  = static_cast<std::size_t>(req.content_length);
            if (already < needed) {
                asio::read(sock, buf, asio::transfer_exactly(needed - already), ec);
                if (ec && ec != asio::error::eof) return;
            }
            req.body = std::string{
                asio::buffers_begin(buf.data()),
                asio::buffers_begin(buf.data()) + static_cast<std::ptrdiff_t>(
                    std::min(buf.size(), needed))};
        }

        if (req.method == "POST" && req.path == "/events") {
            try {
                auto j = json::parse(req.body);
                fre::Event event{};
                std::string tenant_id  = j.value("tenant_id",  "");
                std::string entity_id  = j.value("entity_id",  "");
                std::string event_type = j.value("event_type", "event");
                event.tenant_id  = tenant_id;
                event.entity_id  = entity_id;
                event.event_type = event_type;

                auto result = impl.pipeline.submit(event);
                if (result.has_value()) {
                    asio::write(sock, asio::buffer(
                        http_response(202, "Accepted", "text/plain", "")), ec);
                } else {
                    const std::string msg = fre::error_message(result.error());
                    asio::write(sock, asio::buffer(
                        http_response(503, "Service Unavailable", "text/plain", msg)), ec);
                }
            } catch (const json::parse_error& e) {
                asio::write(sock, asio::buffer(
                    http_response(400, "Bad Request", "text/plain", e.what())), ec);
            }

        } else if (req.method == "GET" && req.path == "/health") {
            const auto state = impl.pipeline.state();
            const std::string state_str = [state]() -> std::string {
                switch (state) {
                    case fre::PipelineState::Stopped:  return "stopped";
                    case fre::PipelineState::Starting: return "starting";
                    case fre::PipelineState::Running:  return "running";
                    case fre::PipelineState::Draining: return "draining";
                }
                return "unknown";
            }();

            json health;
            health["state"]    = state_str;
            health["pipeline"] = std::string{impl.pipeline.pipeline_id()};
            health["degraded"] = false;

            asio::write(sock, asio::buffer(
                http_response(200, "OK", "application/json", health.dump())), ec);

        } else if (req.method == "POST" && req.path == "/pipeline/drain") {
            impl.running.store(false, std::memory_order_release);
            impl.pipeline.drain(std::chrono::milliseconds{5000});

            json body_j;
            body_j["status"] = "drained";
            asio::write(sock, asio::buffer(
                http_response(200, "OK", "application/json", body_j.dump())), ec);

        } else {
            asio::write(sock, asio::buffer(
                http_response(404, "Not Found", "text/plain", "not found")), ec);
        }

    } catch (const std::exception&) {}
}

// ─── ServiceHarness ──────────────────────────────────────────────────────────

ServiceHarness::ServiceHarness(HarnessConfig config)
    : impl_{std::make_unique<Impl>(std::move(config))}
{}

ServiceHarness::~ServiceHarness() { stop(); }

bool ServiceHarness::start() {
    auto result = impl_->pipeline.start();
    if (!result.has_value()) {
        std::cerr << "[fre-service] pipeline start failed: "
                  << fre::error_message(result.error()) << "\n";
        return false;
    }

    asio::ip::tcp::endpoint ep{
        asio::ip::make_address(impl_->config.bind_address),
        impl_->config.port};

    asio::error_code ec;
    impl_->acceptor.open(ep.protocol(), ec);
    if (ec) { std::cerr << "[fre-service] open: " << ec.message() << "\n"; return false; }
    impl_->acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    impl_->acceptor.bind(ep, ec);
    if (ec) { std::cerr << "[fre-service] bind: " << ec.message() << "\n"; return false; }
    impl_->acceptor.listen(asio::ip::tcp::socket::max_listen_connections, ec);
    if (ec) { std::cerr << "[fre-service] listen: " << ec.message() << "\n"; return false; }

    impl_->running.store(true, std::memory_order_release);

    impl_->io_thread = std::thread([this] {
        while (impl_->running.load(std::memory_order_acquire)) {
            asio::error_code ec;
            asio::ip::tcp::socket sock{impl_->ioc};
            impl_->acceptor.accept(sock, ec);
            if (ec) break;
            std::thread{Impl::handle_connection, std::move(sock), std::ref(*impl_)}.detach();
        }
    });

    std::cout << "[fre-service] listening on "
              << impl_->config.bind_address << ":" << impl_->config.port << "\n";
    return true;
}

void ServiceHarness::run() {
    while (impl_->running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
}

void ServiceHarness::stop() {
    if (!impl_) return;
    impl_->running.store(false, std::memory_order_release);
    impl_->acceptor.close();
    if (impl_->io_thread.joinable()) {
        impl_->io_thread.join();
    }
    impl_->pipeline.drain(std::chrono::milliseconds{5000});
}

// ─── load_config_from_json ───────────────────────────────────────────────────

std::optional<PipelineConfig> load_config_from_json(const std::string& path) {
    std::ifstream ifs{path};
    if (!ifs.is_open()) {
        std::cerr << "[fre-service] cannot open config: " << path << "\n";
        return std::nullopt;
    }

    try {
        auto j = json::parse(ifs);
        PipelineConfig config;
        config.pipeline_id      = j.value("pipeline_id",      "fre-service");
        config.pipeline_version = j.value("pipeline_version", "1.0.0");
        config.emit_config.add_stdout_target();
        return config;
    } catch (const json::parse_error& e) {
        std::cerr << "[fre-service] config parse error: " << e.what() << "\n";
        return std::nullopt;
    }
}

}  // namespace fre::service
