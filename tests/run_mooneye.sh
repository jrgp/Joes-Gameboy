#!/bin/bash
cd "$(dirname "$0")/.."
PASS=0; FAIL=0; FAILLIST=()
for rom in roms/mooneye/acceptance/**/*.gb roms/mooneye/acceptance/*.gb; do
    [[ -f "$rom" ]] || continue
    name="${rom#roms/mooneye/acceptance/}"
    out=$(./gb --headless --cycles 20000000 "$rom" 2>/dev/null)
    if echo "$out" | grep -q "Passed"; then
        PASS=$((PASS+1))
    else
        FAIL=$((FAIL+1))
        FAILLIST+=("$name")
    fi
done
for f in "${FAILLIST[@]}"; do printf '  [FAIL] %s\n' "$f"; done
echo "=== Mooneye: $PASS/$((PASS+FAIL)) passed ==="
