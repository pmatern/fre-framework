#!/usr/bin/env bash
# check-coverage.sh — Enforce 80% line coverage threshold (FR-017).
# Parses lcov --summary output from build/coverage.info.
# Exit code: 0 if coverage >= threshold; 1 if below threshold or data missing.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
COVERAGE_INFO="${1:-${REPO_ROOT}/build/coverage/coverage.info}"
THRESHOLD="${2:-80.0}"

if [[ ! -f "${COVERAGE_INFO}" ]]; then
    echo "ERROR: coverage data not found at ${COVERAGE_INFO}"
    echo "       Run 'cmake --build --preset coverage --target coverage' first."
    exit 1
fi

echo "=== [coverage] parsing ${COVERAGE_INFO} ==="

# lcov --summary output contains a line like:
#   lines......: 83.4% (1234 of 1480 lines)
SUMMARY=$(lcov --summary "${COVERAGE_INFO}" 2>&1)
echo "${SUMMARY}"

LINE_COVERAGE=$(echo "${SUMMARY}" \
    | grep -E "lines\.*:" \
    | grep -oE "[0-9]+\.[0-9]+" \
    | head -1)

if [[ -z "${LINE_COVERAGE}" ]]; then
    echo "ERROR: could not parse line coverage percentage from lcov output."
    exit 1
fi

echo ""
echo "=== [coverage] line coverage: ${LINE_COVERAGE}% (threshold: ${THRESHOLD}%) ==="

# Use awk for floating-point comparison (bash can only do integer arithmetic).
PASS=$(awk -v actual="${LINE_COVERAGE}" -v threshold="${THRESHOLD}" \
    'BEGIN { print (actual >= threshold) ? "1" : "0" }')

if [[ "${PASS}" == "1" ]]; then
    echo "PASS: coverage ${LINE_COVERAGE}% >= ${THRESHOLD}%"
    exit 0
else
    echo "FAIL: coverage ${LINE_COVERAGE}% < ${THRESHOLD}% — raise coverage or lower threshold"
    exit 1
fi
