#include <fre/evaluator/plugin_loader.hpp>
#include <fre/core/logging.hpp>

#include <chrono>
#include <cmath>
#include <cstring>
#include <format>

#if defined(_WIN32)
#  include <windows.h>
#  define FRE_DLOPEN(path)   LoadLibraryA(path)
#  define FRE_DLSYM(h, sym)  GetProcAddress(static_cast<HMODULE>(h), sym)
#  define FRE_DLCLOSE(h)     FreeLibrary(static_cast<HMODULE>(h))
#  define FRE_DLERROR()      "Windows error"
#else
#  include <dlfcn.h>
#  define FRE_DLOPEN(path)   dlopen(path, RTLD_NOW | RTLD_LOCAL)
#  define FRE_DLSYM(h, sym)  dlsym(h, sym)
#  define FRE_DLCLOSE(h)     dlclose(h)
#  define FRE_DLERROR()      dlerror()
#endif

namespace fre {

// ─── EvaluatorHandle ─────────────────────────────────────────────────────────

EvaluatorHandle::EvaluatorHandle(fre_evaluator_vtable_t* vtable, void* dl_handle)
    : vtable_{vtable}, dl_handle_{dl_handle} {}

EvaluatorHandle::~EvaluatorHandle() {
    if (vtable_ && vtable_->destroy) {
        vtable_->destroy(vtable_->ctx);
        delete vtable_;
    }
    if (dl_handle_) {
        FRE_DLCLOSE(dl_handle_);
    }
}

EvaluatorHandle::EvaluatorHandle(EvaluatorHandle&& other) noexcept
    : vtable_{std::exchange(other.vtable_, nullptr)}
    , dl_handle_{std::exchange(other.dl_handle_, nullptr)} {}

EvaluatorHandle& EvaluatorHandle::operator=(EvaluatorHandle&& other) noexcept {
    if (this != &other) {
        // destroy current
        if (vtable_ && vtable_->destroy) vtable_->destroy(vtable_->ctx);
        if (dl_handle_) FRE_DLCLOSE(dl_handle_);

        vtable_    = std::exchange(other.vtable_, nullptr);
        dl_handle_ = std::exchange(other.dl_handle_, nullptr);
    }
    return *this;
}

std::string_view EvaluatorHandle::evaluator_id() const noexcept {
    if (!vtable_ || !vtable_->evaluator_id) return {};
    return vtable_->evaluator_id;
}

std::expected<EvaluatorResult, EvaluatorError> EvaluatorHandle::evaluate(const Event& event) {
    if (!vtable_ || !vtable_->evaluate) {
        return std::unexpected(EvaluatorError{
            .code         = EvaluatorErrorCode::NotInitialized,
            .evaluator_id = std::string{evaluator_id()},
        });
    }

    // Convert fre::Event to C-ABI struct
    fre_event_view_t ev_view{};
    ev_view.id         = event.id;
    ev_view.tenant_id  = event.tenant_id.data();
    ev_view.entity_id  = event.entity_id.data();
    ev_view.event_type = event.event_type.data();
    ev_view.timestamp_us = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            event.timestamp.time_since_epoch()).count());
    ev_view.payload     = event.payload.data();
    ev_view.payload_len = static_cast<uint32_t>(event.payload.size());

    fre_evaluator_result_t result{};
    const int rc = vtable_->evaluate(vtable_->ctx, &ev_view, &result);

    if (rc != 0 || result.is_error) {
        return std::unexpected(EvaluatorError{
            .code         = static_cast<EvaluatorErrorCode>(result.error_code),
            .evaluator_id = result.evaluator_id,
            .detail       = result.reason_code,
        });
    }

    EvaluatorResult out;
    out.evaluator_id = result.evaluator_id;
    out.verdict      = static_cast<Verdict>(result.verdict);
    if (result.reason_code[0] != '\0') out.reason_code = result.reason_code;
    if (!std::isnan(result.score))     out.score        = result.score;
    return out;
}

// ─── PluginLoader ─────────────────────────────────────────────────────────────

std::expected<EvaluatorHandle, Error> PluginLoader::load(
    const std::string& library_path,
    const std::string& config_json) {

    void* handle = FRE_DLOPEN(library_path.c_str());
    if (!handle) {
        return std::unexpected(Error{ConfigError{
            ConfigErrorCode::EvaluatorLoadFailed,
            std::format("dlopen '{}': {}", library_path, FRE_DLERROR())
        }});
    }

    auto* factory = reinterpret_cast<fre_create_evaluator_fn>(
        FRE_DLSYM(handle, "fre_create_evaluator"));
    if (!factory) {
        FRE_DLCLOSE(handle);
        return std::unexpected(Error{ConfigError{
            ConfigErrorCode::EvaluatorLoadFailed,
            std::format("symbol 'fre_create_evaluator' not found in '{}'", library_path)
        }});
    }

    fre_evaluator_vtable_t* vtable = factory(config_json.c_str());
    if (!vtable) {
        FRE_DLCLOSE(handle);
        return std::unexpected(Error{ConfigError{
            ConfigErrorCode::EvaluatorLoadFailed,
            std::format("factory returned null for '{}'", library_path)
        }});
    }

    if (vtable->abi_version != FRE_EVALUATOR_ABI_VERSION) {
        if (vtable->destroy) vtable->destroy(vtable->ctx);
        delete vtable;
        FRE_DLCLOSE(handle);
        return std::unexpected(Error{ConfigError{
            ConfigErrorCode::EvaluatorLoadFailed,
            std::format("plugin ABI version {} != expected {} in '{}'",
                        vtable->abi_version, FRE_EVALUATOR_ABI_VERSION, library_path)
        }});
    }

    FRE_LOG_INFO("Loaded evaluator plugin '{}' from '{}'",
                 vtable->evaluator_id ? vtable->evaluator_id : "<unknown>",
                 library_path);

    return EvaluatorHandle{vtable, handle};
}

}  // namespace fre
