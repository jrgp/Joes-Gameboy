#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
GB="$ROOT/gb"

# Default cycle limit: ~12 seconds of emulated time at ~4MHz
# blargg cpu_instrs tests need up to ~10s to complete
CYCLES="${GB_TEST_CYCLES:-100000000}"

pass=0
fail=0
total=0

if [ ! -x "$GB" ]; then
    echo "ERROR: $GB not found or not executable. Run 'make' first." >&2
    exit 1
fi

run_rom() {
    local suite="$1"
    local rom="$2"
    local name
    name="$(basename "$rom")"
    total=$((total + 1))

    # Run headless with cycle cap, capture stdout (serial output goes there)
    output=$("$GB" --headless --cycles "$CYCLES" "$rom" 2>/dev/null || true)

    if echo "$output" | grep -qi "passed"; then
        status="PASS"
        pass=$((pass + 1))
    elif echo "$output" | grep -qi "failed"; then
        status="FAIL"
        fail=$((fail + 1))
    else
        status="UNKNOWN"
        fail=$((fail + 1))
    fi

    printf "  [%-7s] %s/%s\n" "$status" "$suite" "$name"
    if [ "$status" != "PASS" ] && [ -n "$output" ]; then
        echo "$output" | grep -v "^\[headless\]\|^Loaded\|^bios disabled\|^SDL" | tail -5 | sed 's/^/             /'
    fi
}

run_suite() {
    local suite="$1"
    local target="$2"
    if [ -f "$target" ]; then
        echo "--- $suite ---"
        run_rom "$suite" "$target"
        echo ""
    elif [ -d "$target" ]; then
        echo "--- $suite ---"
        while IFS= read -r -d '' rom; do
            run_rom "$suite" "$rom"
        done < <(find "$target" -name "*.gb" -print0 | sort -z)
        echo ""
    fi
}

echo "=== blargg test suites ==="
echo "Binary : $GB"
echo "Cycles : $CYCLES"
echo ""

run_suite "cpu_instrs"   "$ROOT/roms/blargg"
run_suite "mem_timing"   "$ROOT/roms/mem_timing"
run_suite "mem_timing-2" "$ROOT/roms/mem_timing-2"
run_suite "instr_timing" "$ROOT/roms/instr_timing"
run_suite "halt_bug"     "$ROOT/roms/halt_bug.gb"
run_suite "interrupt_time" "$ROOT/roms/interrupt_time"
run_suite "oam_bug"      "$ROOT/roms/oam_bug"

echo "=== Results: $pass/$total passed ==="

[ $fail -eq 0 ]
