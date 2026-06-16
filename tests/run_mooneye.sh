#!/bin/bash
cd "$(dirname "$0")/.."
PASS=0; FAIL=0; FAILLIST=()

# Infer --model flag from ROM filename suffix.
# e.g. boot_regs-dmg0.gb → --model dmg0
#      boot_regs-mgb.gb  → --model mgb
#      boot_div-S.gb     → --model S (GBC)
get_model_flag() {
    local rom="$1"
    local base
    base=$(basename "$rom" .gb)
    # Extract the suffix after the last '-'
    local suffix="${base##*-}"
    # boot_div2-S tests SGB2; boot_div-S/boot_hwio-S test SGB1.
    if [[ "$base" == *"2-S" ]]; then
        echo "--model sgb2"
        return
    fi
    case "$suffix" in
        dmg0)         echo "--model dmg0" ;;
        mgb)          echo "--model mgb" ;;
        sgb)          echo "--model sgb" ;;
        sgb2)         echo "--model sgb2" ;;
        S)            echo "--model sgb" ;;
        *)            echo "" ;;   # default (DMG)
    esac
}

for rom in tests/assets/mooneye-test-suite/acceptance/**/*.gb tests/assets/mooneye-test-suite/acceptance/*.gb; do
    [[ -f "$rom" ]] || continue
    name="${rom#roms/mooneye/acceptance/}"
    model_flag=$(get_model_flag "$rom")
    out=$(./gb --headless --cycles 20000000 $model_flag "$rom" 2>/dev/null)
    if echo "$out" | grep -q "Passed"; then
        PASS=$((PASS+1))
    else
        FAIL=$((FAIL+1))
        FAILLIST+=("$name")
    fi
done
for f in "${FAILLIST[@]}"; do printf '  [FAIL] %s\n' "$f"; done
echo "=== Mooneye: $PASS/$((PASS+FAIL)) passed ==="
