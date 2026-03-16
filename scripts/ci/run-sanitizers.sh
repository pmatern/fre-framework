#!/usr/bin/env bash
# run-sanitizers.sh — Build and test under ASan, UBSan, and TSan.
# All three presets must pass clean for the FR-016 quality gate to pass.
# Exit code: 0 if all three pass; non-zero on first failure.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LOG_DIR="${REPO_ROOT}/build/sanitizer-logs"
mkdir -p "${LOG_DIR}"

run_sanitizer() {
    local preset="$1"
    local log_file="${LOG_DIR}/${preset}.log"

    echo "=== [sanitizer] configuring preset: ${preset} ==="
    cmake --preset "${preset}" 2>&1 | tee "${log_file}"

    echo "=== [sanitizer] building preset: ${preset} ==="
    cmake --build --preset "${preset}" 2>&1 | tee -a "${log_file}"

    echo "=== [sanitizer] running tests under: ${preset} ==="
    if ! ctest --preset "${preset}" 2>&1 | tee -a "${log_file}"; then
        echo ""
        echo "FAIL: sanitizer preset '${preset}' — one or more tests failed."
        echo "      See ${log_file} for details."
        return 1
    fi

    echo "PASS: sanitizer preset '${preset}'"
}

FAILED=0

for preset in asan ubsan tsan; do
    if ! run_sanitizer "${preset}"; then
        FAILED=1
        # Continue running remaining presets so all failures are reported.
    fi
done

if [[ ${FAILED} -ne 0 ]]; then
    echo ""
    echo "=== [sanitizer] GATE FAILED — see logs in ${LOG_DIR}/ ==="
    exit 1
fi

echo ""
echo "=== [sanitizer] GATE PASSED — ASan, UBSan, TSan all clean ==="
exit 0
