#!/usr/bin/env bash
# run_cgb_tests.sh — Run GBC-specific test ROMs and report pass/fail.
#
# Tests are drawn from tests/assets/ (downloaded by make test-assets).
# Separate sections for each suite make DMG vs GBC progress easy to track.
#
# Usage: ./tests/run_cgb_tests.sh
#        or via: make test (after make test-assets)
set -euo pipefail

cd "$(dirname "$0")/.."
GB="./gb"
ASSETS="tests/assets"
PASS=0; FAIL=0

if [ ! -x "$GB" ]; then
    echo "ERROR: $GB not found. Run 'make' first." >&2
    exit 1
fi

if [ ! -d "$ASSETS" ]; then
    echo "ERROR: $ASSETS not found. Run 'make test-assets' first." >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# Infer --model flag from ROM filename suffix (e.g. boot_regs-cgb0.gb → cgb0)
get_cgb_model_flag() {
    local rom="$1"
    local base; base=$(basename "$rom" .gb); base=$(basename "$base" .gbc)
    local suffix="${base##*-}"
    case "$suffix" in
        cgb0) echo "--model cgb" ;;   # CGB0 — closest we have is generic CGB
        cgbABCDE|cgbC|cgbBCE|cgbBC|cgbBCE|cgbDE|cgbBCD) echo "--model cgb" ;;
        C)    echo "--model cgb" ;;
        *)    echo "--model cgb" ;;   # default to CGB for this script
    esac
}

run_rom_serial() {
    local suite="$1" rom="$2" model_flag="${3:-}"
    local name; name="$(basename "$rom")"
    local out; out=$("$GB" --headless --cycles 100000000 $model_flag "$rom" 2>/dev/null || true)
    if echo "$out" | grep -qi "passed"; then
        printf "  [PASS   ] %s/%s\n" "$suite" "$name"
        PASS=$((PASS+1))
    elif echo "$out" | grep -qi "failed"; then
        printf "  [FAIL   ] %s/%s\n" "$suite" "$name"
        FAIL=$((FAIL+1))
    else
        printf "  [UNKNOWN] %s/%s\n" "$suite" "$name"
        FAIL=$((FAIL+1))
    fi
}

run_rom_mooneye() {
    local suite="$1" rom="$2"
    local name; name="$(basename "$rom")"
    local model_flag; model_flag=$(get_cgb_model_flag "$rom")
    local out; out=$("$GB" --headless --cycles 20000000 $model_flag "$rom" 2>/dev/null || true)
    if echo "$out" | grep -q "Passed"; then
        printf "  [PASS   ] %s/%s\n" "$suite" "$name"
        PASS=$((PASS+1))
    else
        printf "  [FAIL   ] %s/%s\n" "$suite" "$name"
        FAIL=$((FAIL+1))
    fi
}

# ---------------------------------------------------------------------------
# GBC Tests
# ---------------------------------------------------------------------------

echo "=== GBC Tests ==="
echo "Binary : $GB"
echo ""

# --- Mooneye misc (CGB-specific timing/register tests) ---
echo "--- mooneye/misc (CGB) ---"
for rom in "$ASSETS/mooneye-test-suite/misc"/*.gb \
           "$ASSETS/mooneye-test-suite/misc"/**/*.gb; do
    [[ -f "$rom" ]] || continue
    run_rom_mooneye "mooneye/misc" "$rom"
done
echo ""

# --- Blargg cgb_sound ---
echo "--- blargg/cgb_sound (individual) ---"
for rom in "$ASSETS/blargg/cgb_sound/rom_singles"/*.gb; do
    [[ -f "$rom" ]] || continue
    run_rom_serial "cgb_sound" "$rom" "--model cgb"
done
echo ""

# --- cgb-acid2 (visual; no automated pass/fail — always listed as UNKNOWN) ---
echo "--- cgb-acid2 (visual PPU test — no automated pass/fail) ---"
if [[ -f "$ASSETS/cgb-acid2/cgb-acid2.gbc" ]]; then
    printf "  [VISUAL] cgb-acid2/cgb-acid2.gbc (inspect manually)\n"
fi
echo ""

echo "=== GBC Results: $PASS passed, $FAIL failed, $((PASS+FAIL)) total ==="

# Exit 0 regardless — GBC tests are expected to fail during early development.
# Change to `[ $FAIL -eq 0 ]` once GBC is mature.
exit 0
