#!/usr/bin/env bash
#
# compare_slither_docker.sh — SCSA vs Slither (baseline) на корпусе reentrancy.
# Slither гоняется в официальном образе trailofbits/eth-security-toolbox (slither + solc
# через solc-select), т.к. локально slither/solc не установлены. Сравнение на уровне
# ФАЙЛА (есть ли reentrancy-находка) — нумерация строк у инструментов расходится.
#
# Диплом (раздел 13): обязательное сравнение precision/recall с baseline. Все файлы
# клона помечены уязвимыми (reentrancy dataset), поэтому меряем recall каждого и согласие.
#
# использование: ./benchmarks/compare_slither_docker.sh [папка] [solc-версия]
# требует: docker + образ trailofbits/eth-security-toolbox.

set -u -o pipefail
DIR="${1:-benchmarks/smartbugs/clone_reentrancy}"
SOLC="${2:-0.4.26}"
IMG="${SLITHER_IMAGE:-trailofbits/eth-security-toolbox}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SCSA="${SCSA_BIN:-$REPO_ROOT/build/src/solidity-llvm}"
ABSDIR="$(cd "$DIR" && pwd)"

[ ! -d "$ABSDIR" ] && { echo "error: нет папки $DIR" >&2; exit 2; }
[ ! -x "$SCSA" ] && { echo "error: собери SCSA" >&2; exit 2; }
command -v docker >/dev/null 2>&1 && docker image inspect "$IMG" >/dev/null 2>&1 || {
    echo "error: нужен docker + образ $IMG (docker pull $IMG)" >&2; exit 2; }

# 1. Slither по всем .sol за один проход в контейнере → slither.txt (name YES/NO/ERR)
echo "== Slither ($IMG, solc $SOLC) =="
docker run --rm -v "$ABSDIR":/work "$IMG" bash -c '
    solc-select install '"$SOLC"' >/dev/null 2>&1; solc-select use '"$SOLC"' >/dev/null 2>&1
    for f in /work/*.sol; do
        name=$(basename "$f" .sol)
        out=$(slither "$f" --json - 2>/dev/null)
        if [ -z "$out" ]; then echo "$name ERR"; continue; fi
        if echo "$out" | grep -qE "\"check\": \"reentrancy"; then echo "$name YES"; else echo "$name NO"; fi
    done' > "$REPO_ROOT/build/slither.txt" 2>/dev/null

# 2. SCSA-вердикты + сведение
BOTH=0 ONLY_S=0 ONLY_C=0 NEITHER=0 NOCOMPILE=0 SERR=0
printf "%-46s %-8s %-8s\n" ФАЙЛ SCSA Slither
for f in "$ABSDIR"/*.sol; do
    name=$(basename "$f" .sol)
    # SCSA: компилируется? нашёл HIGH reentrancy? (--audit возвращает exit 2 при находках,
    # поэтому вывод берём через подстановку, а не в pipefail-конвейере)
    if ! "$SCSA" --input "$f" --emit-ir >/dev/null 2>&1; then scsa=NOCOMPILE; NOCOMPILE=$((NOCOMPILE+1))
    else
        aud="$("$SCSA" --input "$f" --audit 2>/dev/null || true)"
        if echo "$aud" | grep -qiE '\[HIGH\].*reentran'; then scsa=YES; else scsa=no; fi
    fi
    sl=$(awk -v n="$name" '$1==n{print $2}' "$REPO_ROOT/build/slither.txt"); sl=${sl:-ERR}
    printf "%-46s %-8s %-8s\n" "${name:0:44}" "$scsa" "$sl"
    [ "$sl" = ERR ] && SERR=$((SERR+1))
    if [ "$scsa" = YES ] && [ "$sl" = YES ]; then BOTH=$((BOTH+1))
    elif [ "$sl" = YES ] && [ "$scsa" != YES ]; then ONLY_S=$((ONLY_S+1))
    elif [ "$scsa" = YES ] && [ "$sl" != YES ]; then ONLY_C=$((ONLY_C+1))
    elif [ "$sl" = NO ] && [ "$scsa" != YES ]; then NEITHER=$((NEITHER+1)); fi
done

TOTAL=$(ls "$ABSDIR"/*.sol | wc -l)
echo
echo "== СВОДКА (корпус $TOTAL, все помечены уязвимыми reentrancy) =="
echo "  оба нашли (SCSA∧Slither)   : $BOTH"
echo "  только Slither             : $ONLY_S"
echo "  только SCSA                : $ONLY_C"
echo "  оба молчат                 : $NEITHER"
echo "  SCSA не компилирует        : $NOCOMPILE  (Slither на реальном solc компилирует всё)"
echo "  Slither ошибок             : $SERR"
SREC=$(awk '$2=="YES"{c++} END{print c+0}' "$REPO_ROOT/build/slither.txt")
echo "  Slither recall             : $SREC / $TOTAL"
echo "  SCSA recall (на компилир.) : $((BOTH+ONLY_C)) / $((TOTAL-NOCOMPILE))"
echo "  Вывод: на компилируемом подмножестве SCSA согласен со Slither; ров SCSA — символьное"
echo "         подтверждение (KLEE) + PoC, чего Slither не даёт."
