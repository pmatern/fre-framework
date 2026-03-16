#pragma once

#include <fre/core/concepts.hpp>
#include <fre/core/error.hpp>
#include <fre/core/plugin_abi.h>

#include <expected>
#include <functional>
#include <memory>
#include <string>

namespace fre {

// ─── EvaluatorHandle ─────────────────────────────────────────────────────────

/// Owns a loaded plugin vtable. Calls destroy() on destruction.
class EvaluatorHandle {
public:
    explicit EvaluatorHandle(fre_evaluator_vtable_t* vtable, void* dl_handle = nullptr);
    ~EvaluatorHandle();

    EvaluatorHandle(const EvaluatorHandle&)            = delete;
    EvaluatorHandle& operator=(const EvaluatorHandle&) = delete;
    EvaluatorHandle(EvaluatorHandle&&) noexcept;
    EvaluatorHandle& operator=(EvaluatorHandle&&) noexcept;

    [[nodiscard]] std::string_view evaluator_id() const noexcept;

    /// Call the plugin's evaluate() and convert to fre types.
    [[nodiscard]] std::expected<EvaluatorResult, EvaluatorError>
    evaluate(const Event& event);

private:
    fre_evaluator_vtable_t* vtable_{nullptr};
    void*                   dl_handle_{nullptr};  // dlopen handle; nullptr for embedded plugins
};

// ─── PluginLoader ────────────────────────────────────────────────────────────

class PluginLoader {
public:
    /// Load a shared library plugin from `library_path`.
    /// Calls `fre_create_evaluator(config_json)` to obtain the vtable.
    /// Returns ConfigError if the library cannot be opened, the factory symbol
    /// is missing, or the ABI version does not match.
    [[nodiscard]] static std::expected<EvaluatorHandle, Error> load(
        const std::string& library_path,
        const std::string& config_json = "{}");
};

}  // namespace fre
