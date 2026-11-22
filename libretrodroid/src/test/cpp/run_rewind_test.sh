#!/usr/bin/env bash
# Builds and runs the Rewind host test through the LLM Coordinator pipeline gate.
# Usage: run_rewind_test.sh <scratch-dir>   (g++ from PATH or $GXX)
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
scratch="${1:?scratch dir}"
gxx="${GXX:-$(command -v g++)}"
gate="$LOCALAPPDATA/Tools/llm-coordinator/bin/coordinator-client.exe"
mkdir -p "$scratch/pipeline"

run() {
    local task="$1"; shift
    local out
    out="$("$gate" gate exec --tool pipeline --memory-limit-mib 1024 --owner-task "$task" \
        --work-directory "$here" --scratch-root "$scratch/pipeline" -- "$@" 2>&1 || true)"
    local ticket
    ticket="$(printf '%s' "$out" | grep -o '"ticket":"[0-9a-f]*"' | tail -1 | cut -d'"' -f4)"
    cat "$scratch/pipeline/$ticket.stdout.log" "$scratch/pipeline/$ticket.stderr.log" 2>/dev/null || true
    printf '%s' "$out" | grep -q '"phase":"COMPLETED"'
}

run "Compile Rewind host test" "$gxx" -std=c++17 -O2 -Wall -Wextra -pthread \
    -Izlibstub -I../../main/cpp \
    rewind_test.cpp ../../main/cpp/rewind.cpp -o "$scratch/rewind_test.exe"
run "Run Rewind host test" "$scratch/rewind_test.exe"
