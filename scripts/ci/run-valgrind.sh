#!/usr/bin/env bash
# run-valgrind.sh — Advisory Valgrind memcheck pass (FR-019).
# Runs memcheck against all test executables in build/debug/tests/.
# Always exits 0 (non-blocking advisory gate); results archived as XML.
# CI should surface the report but NOT block merge on failures.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build/debug"
REPORT_DIR="${REPO_ROOT}/build/valgrind"
SUPP_FILE="${REPO_ROOT}/valgrind.supp"

mkdir -p "${REPORT_DIR}"

if ! command -v valgrind &>/dev/null; then
    echo "[valgrind] valgrind not found — skipping advisory gate (Linux CI only)"
    exit 0
fi

echo "=== [valgrind] starting memcheck advisory pass ==="
echo "    build dir : ${BUILD_DIR}"
echo "    report dir: ${REPORT_DIR}"
echo "    suppressions: ${SUPP_FILE}"

FAIL_COUNT=0
PASS_COUNT=0

# Find all test executables (non-hidden, executable files under tests/).
while IFS= read -r -d '' TEST_BIN; do
    NAME="$(basename "${TEST_BIN}")"
    XML_OUT="${REPORT_DIR}/${NAME}.xml"
    LOG_OUT="${REPORT_DIR}/${NAME}.log"

    echo ""
    echo "--- [valgrind] ${NAME} ---"

    SUPP_ARG=""
    if [[ -f "${SUPP_FILE}" ]]; then
        SUPP_ARG="--suppressions=${SUPP_FILE}"
    fi

    # --error-exitcode=0: advisory — never fail CI
    valgrind \
        --tool=memcheck \
        --leak-check=full \
        --show-leak-kinds=definite,indirect \
        --track-origins=yes \
        --error-exitcode=0 \
        ${SUPP_ARG} \
        --xml=yes \
        --xml-file="${XML_OUT}" \
        "${TEST_BIN}" --reporter compact 2>"${LOG_OUT}" || true

    # Count definite errors from XML for the summary report.
    ERRORS=$(grep -c '<kind>.*Leak\|UninitCondition\|InvalidRead\|InvalidWrite' "${XML_OUT}" 2>/dev/null || echo 0)
    if [[ "${ERRORS}" -gt 0 ]]; then
        echo "  ADVISORY: ${ERRORS} potential issue(s) detected — see ${XML_OUT}"
        (( FAIL_COUNT += 1 )) || true
    else
        echo "  clean"
        (( PASS_COUNT += 1 )) || true
    fi

done < <(find "${BUILD_DIR}/tests" -maxdepth 3 -type f -perm +111 -print0 2>/dev/null)

echo ""
echo "=== [valgrind] advisory summary: ${PASS_COUNT} clean, ${FAIL_COUNT} with findings ==="
echo "    Reports: ${REPORT_DIR}/"
echo "    NOTE: this gate is non-blocking (always exits 0)"
exit 0
