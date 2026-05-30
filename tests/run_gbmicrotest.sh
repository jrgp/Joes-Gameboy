#!/bin/bash
# Run GBMicrotest ROMs that use the HRAM pass/fail mechanism (0xFF82=0x01 pass, 0xFF=fail)
cd "$(dirname "$0")/.."
PASS=0; FAIL=0; UNKNOWN=0; FAILLIST=()

for rom in roms/gbmicrotest/*.gb; do
    [[ -f "$rom" ]] || continue
    name=$(basename "$rom")
    out=$(./gb --headless --gbmicrotest --cycles 500000 "$rom" 2>/dev/null)
    if echo "$out" | grep -q "^Passed"; then
        PASS=$((PASS+1))
    elif echo "$out" | grep -q "^Failed"; then
        FAIL=$((FAIL+1))
        detail=$(echo "$out" | grep "^Failed")
        FAILLIST+=("$name: $detail")
    else
        UNKNOWN=$((UNKNOWN+1))
    fi
done

for f in "${FAILLIST[@]}"; do printf '  [FAIL] %s\n' "$f"; done
echo "=== GBMicrotest: $PASS/$((PASS+FAIL+UNKNOWN)) passed ($FAIL failed, $UNKNOWN unknown) ==="
