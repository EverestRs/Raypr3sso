#!/usr/bin/env bash
# Enforce clang-format on C/C++ files ADDED since a base ref.
#
# The legacy tree predates .clang-format, so a whole-repo check would drown in
# pre-existing diffs. New files must be clean; touched legacy files can be
# formatted opportunistically without making the check fail on untouched code.
#
# Usage: scripts/ci/format_check.sh [base-ref]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

BASE="${1:-}"
# Accept only SHA-like refs: the value can come from CI event payloads.
if ! [[ "$BASE" =~ ^[0-9a-fA-F]{7,40}$ ]]; then
    BASE=""
fi
if [ -z "$BASE" ] || ! git rev-parse --verify --quiet "${BASE}^{commit}" >/dev/null; then
    if git rev-parse --verify --quiet origin/main >/dev/null; then
        BASE=origin/main
    elif git rev-parse --verify --quiet HEAD~1 >/dev/null; then
        BASE=HEAD~1
    else
        echo "[format] no usable base ref; skipping"
        exit 0
    fi
fi

if ! command -v clang-format >/dev/null 2>&1; then
    echo "[format] clang-format not installed; skipping"
    exit 0
fi

mapfile -t FILES < <(git diff --name-only --diff-filter=A "$BASE" HEAD -- \
    '*.cpp' '*.cc' '*.c' '*.h' '*.hpp' '*.cu' '*.cuh' 2>/dev/null || true)

if [ "${#FILES[@]}" -eq 0 ]; then
    echo "[format] no files added since $BASE; skipping"
    exit 0
fi

echo "[format] checking ${#FILES[@]} added file(s) since $BASE"
clang-format --dry-run --Werror "${FILES[@]}"
echo "[format] OK"