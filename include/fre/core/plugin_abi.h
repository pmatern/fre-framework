/**
 * fre plugin ABI — C header, no C++ types.
 *
 * Evaluator plugins must export:
 *   fre_evaluator_vtable_t* fre_create_evaluator(const char* config_json)
 *
 * The returned vtable must remain valid until destroy() is called.
 * Embed abi_version as the first field; hosts reject plugins where
 * abi_version != FRE_EVALUATOR_ABI_VERSION.
 */
#ifndef FRE_PLUGIN_ABI_H
#define FRE_PLUGIN_ABI_H

#include <stdint.h>

// NOLINTBEGIN(readability-identifier-naming) — C header uses C naming conventions

#ifdef __cplusplus
extern "C" {
#endif

/* ─── ABI version ─────────────────────────────────────────────────────────── */

#define FRE_EVALUATOR_ABI_VERSION 1

/* ─── Wire types (opaque to the host) ─────────────────────────────────────── */

/** Verdict codes matching fre::Verdict enum values */
typedef enum fre_verdict {
    FRE_VERDICT_PASS  = 0,
    FRE_VERDICT_FLAG  = 1,
    FRE_VERDICT_BLOCK = 2,
} fre_verdict_t;

/** Error codes matching fre::EvaluatorErrorCode */
typedef enum fre_evaluator_error_code {
    FRE_EVAL_ERR_TIMEOUT         = 0,
    FRE_EVAL_ERR_INTERNAL        = 1,
    FRE_EVAL_ERR_INVALID_INPUT   = 2,
    FRE_EVAL_ERR_NOT_INITIALIZED = 3,
} fre_evaluator_error_code_t;

/** Serialized event passed to plugin (all strings are null-terminated UTF-8) */
typedef struct fre_event_view {
    uint64_t    id;
    const char* tenant_id;
    const char* entity_id;
    const char* event_type;
    int64_t     timestamp_us;  /* microseconds since Unix epoch */
    const void* payload;
    uint32_t    payload_len;
} fre_event_view_t;

/** Result returned by plugin evaluate() */
typedef struct fre_evaluator_result {
    fre_verdict_t verdict;
    int           is_error;             /* 0 = success, 1 = error */
    fre_evaluator_error_code_t error_code; /* valid only when is_error = 1 */
    char  evaluator_id[64];
    char  reason_code[128];
    float score;                        /* [0.0, 1.0]; NaN if not applicable */
} fre_evaluator_result_t;

/* ─── Vtable ───────────────────────────────────────────────────────────────── */

typedef struct fre_evaluator_vtable {
    /** Must equal FRE_EVALUATOR_ABI_VERSION — checked at load time. */
    uint32_t abi_version;

    /** Null-terminated evaluator identifier. */
    const char* evaluator_id;

    /** Opaque context pointer owned by the plugin. Passed to all callbacks. */
    void* ctx;

    /**
     * Evaluate a single event.
     * Returns 0 on success, non-zero on internal error.
     * `out` is always written (even on error).
     */
    int (*evaluate)(
        void*                   ctx,
        const fre_event_view_t* event,
        fre_evaluator_result_t* out
    );

    /** Release all resources. Called once when the plugin is unloaded. */
    void (*destroy)(void* ctx);
} fre_evaluator_vtable_t;

/* ─── Factory function signature ───────────────────────────────────────────── */

/**
 * Plugin entry point — must be exported with this exact name.
 * config_json: null-terminated JSON configuration string (may be NULL).
 * Returns a heap-allocated vtable on success, NULL on failure.
 */
typedef fre_evaluator_vtable_t* (*fre_create_evaluator_fn)(const char* config_json);

#ifdef __cplusplus
}
#endif

// NOLINTEND(readability-identifier-naming)

#endif /* FRE_PLUGIN_ABI_H */
