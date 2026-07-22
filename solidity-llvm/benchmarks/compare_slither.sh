#!/usr/bin/env bash
#
# compare_slither.sh — сравнение SCSA со Slither (baseline) по reentrancy на общем
# корпусе .sol. Строит таблицу согласия: оба нашли / только Slither / только SCSA /
# оба молчат, и итоговые числа. Нужно для защиты диплома («почему не Slither»).
#
# методология: сравниваем на уровне ФАЙЛА (есть ли reentrancy-находка), т.к. номера
# строк у инструментов различаются. Slither-детекторы reentrancy: reentrancy-eth,
# reentrancy-no-eth, reentrancy-benign, reentrancy-events. SCSA: rule=="reentrancy"
# severity HIGH/MEDIUM. Файлы, которые SCSA не компилирует, выносим отдельно (subset).
#
# использование: ./benchmarks/compare_slither.sh <папка-с-.sol>
# требует: slither (pip install slither-analyzer) + solc в PATH.

set -u -o pipefail
SOL_DIR="${1:-}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SCSA="${SCSA_BIN:-$REPO_ROOT/build/src/solidity-llvm}"

[ -z "$SOL_DIR" ] && { echo "usage: $0 <sol_dir>" >&2; exit 2; }
[ ! -d "$SOL_DIR" ] && { echo "error: нет папки $SOL_DIR" >&2; exit 2; }
if ! command -v slither >/dev/null 2>&1; then
    echo "error: slither не установлен. Установи: pip install slither-analyzer" >&2
    echo "       (нужен также solc, напр. через solc-select)" >&2
    exit 2
fi
[ ! -x "$SCSA" ] && { echo "error: собери SCSA (cmake --build build) или задай SCSA_BIN" >&2; exit 2; }
export PATH="/usr/lib/llvm-18/bin:$PATH"

BOTH=0 ONLY_S=0 ONLY_C=0 NEITHER=0 NOCOMPILE=0
echo "SCSA vs Slither — reentrancy, корпус: $SOL_DIR"
echo

# нашёл ли Slither reentrancy в файле
slither_reentrancy() {
    local out
    out="$(slither "$1" --json - 2>/dev/null)" || return 2   # slither упал
    printf '%s' "$out" | grep -Eq '"check":\s*"reentrancy-(eth|no-eth|benign|events)"'
}
# нашёл ли SCSA reentrancy (HIGH/MEDIUM). rc==1 => не скомпилировалось
scsa_reentrancy() {
    local out rc
    out="$("$SCSA" --input "$1" --audit --json 2>&1)"; rc=$?
    [ $rc -eq 1 ] && return 2
    printf '%s' "$out" | grep -Eq '"rule":\s*"reentrancy".*"severity":\s*"(HIGH|MEDIUM)"' \
        || printf '%s' "$out" | grep -Eq '"severity":\s*"(HIGH|MEDIUM)"[^}]*"rule":\s*"reentrancy"'
}

for f in "$SOL_DIR"/*.sol; do
    [ -e "$f" ] || continue
    scsa_reentrancy "$f"; c=$?
    if [ $c -eq 2 ]; then NOCOMPILE=$((NOCOMPILE+1)); echo "  NOCOMPILE | $(basename "$f")"; continue; fi
    slither_reentrancy "$f"; s=$?
    [ $s -eq 2 ] && s=1   # slither упал => считаем «не нашёл» (консервативно)
    if   [ $c -eq 0 ] && [ $s -eq 0 ]; then BOTH=$((BOTH+1));     tag="BOTH   ";
    elif [ $c -eq 1 ] && [ $s -eq 0 ]; then ONLY_S=$((ONLY_S+1)); tag="SLITHER";
    elif [ $c -eq 0 ] && [ $s -eq 1 ]; then ONLY_C=$((ONLY_C+1)); tag="SCSA   ";
    else NEITHER=$((NEITHER+1));                                  tag="NEITHER"; fi
    echo "  $tag | $(basename "$f")"
done

echo
echo "--- сводка (только скомпилированные SCSA) ---"
echo "  оба нашли      : $BOTH"
echo "  только Slither : $ONLY_S   (SCSA пропустил — кандидаты в FN)"
echo "  только SCSA    : $ONLY_C   (Slither пропустил — потенциальное преимущество)"
echo "  оба молчат     : $NEITHER"
echo "  не скомпил.SCSA: $NOCOMPILE (subset, вне сравнения)"
echo
echo "RESULT both=$BOTH only_slither=$ONLY_S only_scsa=$ONLY_C neither=$NEITHER nocompile=$NOCOMPILE"
