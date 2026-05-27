#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
GB="$ROOT/gb"
ROMS_DIR="$ROOT/roms/blargg"

# Default cycle limit: ~12 seconds of emulated time at ~4MHz
# blargg cpu_instrs tests need up to ~10s to complete
CYCLES="${GB_TEST_CYCLES:-50000000}"

pass=0
fail=0
total=0

if [ ! -x "$GB" ]; then
    echo "ERROR: $GB not found or not executable. Run 'make' first." >&2
    exit 1
fi

if [ ! -d "$ROMS_DIR" ]; then
    echo "ERROR: $ROMS_DIR not found." >&2
    exit 1
fi

echo "=== blargg cpu_instrs test suite ==="
echo "Binary : $GB"
echo "ROMs   : $ROMS_DIR"
echo "Cycles : $CYCLES"
echo ""

for rom in "$ROMS_DIR"/*.gb; do
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
        # No verdict — likely emulator crashed, infinite loop hit cycle cap, or unimplemented
        status="UNKNOWN"
        fail=$((fail + 1))
    fi

    printf "  [%-7s] %s\n" "$status" "$name"
    if [ "$status" != "PASS" ] && [ -n "$output" ]; then
        # Print last few lines of serial output for debugging
        echo "$output" | grep -v "^\[headless\]\|^Loaded\|^bios disabled\|^SDL" | tail -5 | sed 's/^/             /'
    fi
done

echo ""
echo "=== Results: $pass/$total passed ==="

[ $fail -eq 0 ]
