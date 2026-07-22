#!/usr/bin/env bash
#
# verify_invariant.sh — ЗАМЫКАЕТ петлю инвариант-харнесса (Фаза B). Генерит Foundry
# stateful-invariant харнесс (solvency/no-overdraw) и РЕАЛЬНО прогоняет его через
# `forge test`. Семантика ОБРАТНА verify_poc.sh: инвариант ДОЛЖЕН держаться на честном
# контракте; если forge его ЛОМАЕТ ([FAIL] + counterexample) — это подтверждённый баг
# (reentrancy / битый учёт / округление) с последовательностью-эксплойтом.
#
# использование: ./benchmarks/verify_invariant.sh <файл.sol> [solvency|inflation]
# требует: docker + образ ghcr.io/foundry-rs/foundry; собранный SCSA; кеш forge-std.
set -u
SOL="${1:-}"
PROP="${2:-solvency}"
case "$PROP" in
    solvency)        SUF="Solvency";;
    inflation)       SUF="Inflation";;
    inflation-erc20) SUF="Erc20Inflation";;
    roundtrip)       SUF="Roundtrip";;
    supply)          SUF="Supply";;
    *) echo "prop: solvency|inflation|inflation-erc20|roundtrip|supply" >&2; exit 2;;
esac
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SCSA="${SCSA_BIN:-$REPO_ROOT/build/src/solidity-llvm}"
FOUNDRY_IMG="${FOUNDRY_IMAGE:-ghcr.io/foundry-rs/foundry:latest}"
FORGE_STD="${FORGE_STD_CACHE:-$REPO_ROOT/../phase0/forge_poc/lib/forge-std}"

[ -z "$SOL" ]    && { echo "usage: $0 <file.sol>" >&2; exit 2; }
[ ! -f "$SOL" ]  && { echo "error: нет файла $SOL" >&2; exit 2; }
[ ! -x "$SCSA" ] && { echo "error: собери SCSA или задай SCSA_BIN" >&2; exit 2; }
command -v docker >/dev/null 2>&1 || { echo "error: нужен docker" >&2; exit 2; }
[ ! -f "$FORGE_STD/src/Test.sol" ] && { echo "error: нет кеша forge-std ($FORGE_STD)" >&2; exit 3; }
SOL_ABS="$(cd "$(dirname "$SOL")" && pwd)/$(basename "$SOL")"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/scsa_inv.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# 1. генерим инвариант-харнесс (в cwd=$WORK кладётся <Contract><Suf>Invariant.t.sol)
( cd "$WORK" && "$SCSA" --input "$SOL_ABS" --emit-invariant --invariant-property "$PROP" >/dev/null 2>&1 )
HAR="$(ls -t "$WORK"/*${SUF}Invariant.t.sol 2>/dev/null | head -1)"
if [ -z "$HAR" ]; then
    echo "РЕЗУЛЬТАТ: харнесс не сгенерирован (нет подходящей находки/формы для $PROP)."
    exit 1
fi
HARBASE="$(basename "$HAR")"
CONTRACT="${HARBASE%${SUF}Invariant.t.sol}"
echo "[1/3] харнесс сгенерирован: $HARBASE (цель $CONTRACT)"

# 2. скаффолдим forge-проект: victim в src/<Contract>.sol, харнесс в test/, forge-std из кеша.
#    fuzz/invariant-параметры — умеренные, чтобы контрпример находился быстро и детерминично
PROJ="$WORK/proj"
mkdir -p "$PROJ/src" "$PROJ/test" "$PROJ/lib"
cat > "$PROJ/foundry.toml" <<'TOML'
[profile.default]
src = "src"
test = "test"
libs = ["lib"]
[invariant]
runs = 64
depth = 32
fail_on_revert = false
TOML
cp -r "$FORGE_STD" "$PROJ/lib/forge-std"
cp "$SOL_ABS" "$PROJ/src/$CONTRACT.sol"
cp "$HAR"     "$PROJ/test/$HARBASE"
echo "[2/3] forge-проект собран: $PROJ"

# 3. РЕАЛЬНЫЙ прогон forge test
echo "[3/3] forge test:"
OUT="$(timeout 240 docker run --rm -v "$PROJ":/w -w /w --entrypoint forge "$FOUNDRY_IMG" \
        test --match-contract "${CONTRACT}${SUF}Invariant" -vv 2>&1)"
echo "$OUT" | grep -E '\[PASS\]|\[FAIL\]|invariant_|Suite result|Compiler run|Error|error\[|counterexample|sequence' | head -30

echo
if ! echo "$OUT" | grep -q 'Compiler run successful'; then
    echo "РЕЗУЛЬТАТ: ❓ не скомпилировалось — харнесс требует доводки (импорты/типы, см. выше)."
    exit 2
elif echo "$OUT" | grep -qiE '"event":"failure"|Suite result: FAILED|[1-9][0-9]* failed|\[FAIL\]|counterexample'; then
    echo "РЕЗУЛЬТАТ: ✅ БАГ ПОДТВЕРЖДЁН — инвариант '$PROP' СЛОМАН. forge нашёл вход/"
    echo "          последовательность, нарушающую свойство ($CONTRACT). Это доказанный баг,"
    echo "          контрпример/причина — в выводе выше."
    exit 0
elif echo "$OUT" | grep -qE '\[PASS\]|Suite result: ok'; then
    echo "РЕЗУЛЬТАТ: ⚪ инвариант '$PROP' ДЕРЖИТСЯ на $CONTRACT (forge не нашёл нарушения за"
    echo "          заданные runs/depth). Не доказательство отсутствия бага, но по этому"
    echo "          свойству контракт устоял."
    exit 1
else
    echo "РЕЗУЛЬТАТ: ❓ неоднозначный вывод forge — см. лог выше."
    exit 2
fi
