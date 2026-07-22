#!/usr/bin/env bash
#
# verify_poc.sh — ЗАМЫКАЕТ петлю PoC (ров продукта, AD-7). Генерит Foundry-PoC на
# локальной жертве (new-режим) и РЕАЛЬНО прогоняет его через `forge test`. Находка
# считается PoC-VERIFIED только если тест ЗЕЛЁНЫЙ (ассерция кражи/захвата прошла) —
# иначе это лишь каркас. До этого шага «рабочий PoC» был непроверяемым текстом.
#
# использование: ./benchmarks/verify_poc.sh <файл.sol>
# требует: docker + образ ghcr.io/foundry-rs/foundry; собранный SCSA; кеш forge-std
#          (phase0/forge_poc/lib/forge-std — клонируется один раз, дальше офлайн).
set -u
SOL="${1:-}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SCSA="${SCSA_BIN:-$REPO_ROOT/build/src/solidity-llvm}"
FOUNDRY_IMG="${FOUNDRY_IMAGE:-ghcr.io/foundry-rs/foundry:latest}"
FORGE_STD="${FORGE_STD_CACHE:-$REPO_ROOT/../phase0/forge_poc/lib/forge-std}"

[ -z "$SOL" ]     && { echo "usage: $0 <file.sol>" >&2; exit 2; }
[ ! -f "$SOL" ]   && { echo "error: нет файла $SOL" >&2; exit 2; }
[ ! -x "$SCSA" ]  && { echo "error: собери SCSA или задай SCSA_BIN" >&2; exit 2; }
command -v docker >/dev/null 2>&1 || { echo "error: нужен docker" >&2; exit 2; }
[ ! -f "$FORGE_STD/src/Test.sol" ] && {
    echo "error: нет кеша forge-std ($FORGE_STD). один раз:" >&2
    echo "  docker run --rm -v <proj>:/w -w /w --entrypoint forge $FOUNDRY_IMG install foundry-rs/forge-std --no-git" >&2
    exit 3; }
SOL_ABS="$(cd "$(dirname "$SOL")" && pwd)/$(basename "$SOL")"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/scsa_verify.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# 1. генерим PoC (в cwd=$WORK кладётся <Contract><Class>PoC.t.sol)
( cd "$WORK" && "$SCSA" --input "$SOL_ABS" --poc >/dev/null 2>&1 )
POC="$(ls -t "$WORK"/*PoC.t.sol 2>/dev/null | head -1)"
if [ -z "$POC" ]; then
    echo "РЕЗУЛЬТАТ: PoC не сгенерирован (нет HIGH reentrancy/access-control находки)."
    exit 1
fi
POCBASE="$(basename "$POC")"
# имя контракта-жертвы и класс из имени файла PoC
if   [[ "$POCBASE" == *ReentrancyPoC.t.sol ]];     then CONTRACT="${POCBASE%ReentrancyPoC.t.sol}";     CLASS="reentrancy";
elif [[ "$POCBASE" == *AccessControlPoC.t.sol ]];  then CONTRACT="${POCBASE%AccessControlPoC.t.sol}";  CLASS="access-control";
else CONTRACT="${POCBASE%PoC.t.sol}"; CLASS="?"; fi
echo "[1/3] PoC сгенерирован: $POCBASE (жертва $CONTRACT, класс $CLASS)"

# 2. скаффолдим forge-проект: victim в src/<Contract>.sol, PoC в test/, forge-std из кеша
PROJ="$WORK/proj"
mkdir -p "$PROJ/src" "$PROJ/test" "$PROJ/lib"
printf '[profile.default]\nsrc="src"\ntest="test"\nlibs=["lib"]\n' > "$PROJ/foundry.toml"
cp -r "$FORGE_STD" "$PROJ/lib/forge-std"
cp "$SOL_ABS" "$PROJ/src/$CONTRACT.sol"      # исходник жертвы (импорт PoC: ../src/<Contract>.sol)
cp "$POC"     "$PROJ/test/$POCBASE"
echo "[2/3] forge-проект собран: $PROJ"

# 3. РЕАЛЬНЫЙ прогон forge test — верификация эксплойта
echo "[3/3] forge test:"
OUT="$(timeout 180 docker run --rm -v "$PROJ":/w -w /w --entrypoint forge "$FOUNDRY_IMG" \
        test -vv 2>&1)"
echo "$OUT" | grep -E '\[PASS\]|\[FAIL\]|Suite result|Compiler run|Error|error\[' | head -20

echo
if echo "$OUT" | grep -q '\[PASS\]'; then
    echo "РЕЗУЛЬТАТ: ✅ PoC-VERIFIED — forge test ЗЕЛЁНЫЙ. Эксплойт реально сработал"
    echo "          ($CONTRACT / $CLASS). Находку можно нести на баунти с рабочим PoC."
    exit 0
else
    echo "РЕЗУЛЬТАТ: ❌ НЕ подтверждён — forge test не прошёл (см. вывод выше)."
    echo "          PoC — каркас, требует ручной доводки (импорты/депозит/типы/суммы)."
    exit 1
fi
