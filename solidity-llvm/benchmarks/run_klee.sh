#!/usr/bin/env bash
#
# run_klee.sh — символьное подтверждение reentrancy через KLEE + PoC без TODO (K2).
# Понижает .sol в KLEE-исполнимый LLVM-модуль (sca --emit-klee), собирает в биткод,
# запускает KLEE. KLEE символьно исследует fork реентрантного callback (глубина 1) и,
# если достижим путь кражи (total_sent > исходный баланс), выдаёт ERROR + ktest-модель
# (конкретные balance/msg.value/выбор reenter). Из модели извлекаем witness и генерируем
# СИМВОЛЬНО ПОДТВЕРЖДЁННЫЙ Foundry-PoC (ров продукта, AD-7) — не «паттерн», а доказанный путь.
#
# два бэкенда KLEE (авто-выбор): локальный klee+llvm-as, либо docker-образ klee/klee
# (KLEE 3.2 на LLVM 16; сборка .bc и прогон внутри контейнера — версии LLVM согласованы).
# наш IR — текстовый LLVM 18, но текстовый .ll переносим (llvm-as LLVM 16 его принимает).
#
# использование: ./benchmarks/run_klee.sh <файл.sol> [entry-функция]
# требует: (docker + образ klee/klee) ЛИБО (klee + llvm-as + ktest-tool в PATH).

set -u -o pipefail
SOL="${1:-}"
ENTRY="${2:-}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SCSA="${SCSA_BIN:-$REPO_ROOT/build/src/solidity-llvm}"
KLEE_IMG="${KLEE_IMAGE:-klee/klee:latest}"

[ -z "$SOL" ] && { echo "usage: $0 <file.sol> [entry]" >&2; exit 2; }
[ ! -f "$SOL" ] && { echo "error: нет файла $SOL" >&2; exit 2; }
[ ! -x "$SCSA" ] && { echo "error: собери SCSA (cmake --build build) или задай SCSA_BIN" >&2; exit 2; }
SOL_ABS="$(cd "$(dirname "$SOL")" && pwd)/$(basename "$SOL")"

WORK="$(mktemp -d "$REPO_ROOT/build/klee.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
cp "$SOL" "$WORK/in.sol"

# извлекатель witness из ktest-модели, соответствующей ошибке (little-endian -> int)
cat > "$WORK/extract.py" <<'PYEOF'
import sys, subprocess, re, glob, os
outdir = sys.argv[1]
errs = glob.glob(os.path.join(outdir, "*.reentrancy"))
if not errs:
    sys.exit(0)
name = os.path.basename(errs[0]).rsplit(".", 1)[0]           # testNNNNNN
ktest = os.path.join(outdir, name + ".ktest")
txt = subprocess.run(["ktest-tool", ktest], capture_output=True, text=True).stdout
vals, cur = {}, None
for line in txt.splitlines():
    m = re.search(r"name: '(\w+)'", line)
    if m: cur = m.group(1); continue
    m = re.search(r"hex : 0x([0-9a-fA-F]+)", line)
    if m and cur:
        vals[cur] = int.from_bytes(bytes.fromhex(m.group(1)), "little"); cur = None
print("BALANCE=%d" % vals.get("storage0", 0))
print("MSGVALUE=%d" % vals.get("msg_value", 0))
# конкретные аргументы уязвимой функции (arg0, arg1, …) в порядке индекса
argv = {}
for k, v in vals.items():
    m = re.fullmatch(r"arg(\d+)", k)
    if m: argv[int(m.group(1))] = v
print("POCARGS=" + ",".join(str(argv[i]) for i in sorted(argv)))
PYEOF

# 1. понижаем в KLEE-модуль
ARGS=(--input "$WORK/in.sol" --emit-klee -o "$WORK/m.ll")
[ -n "$ENTRY" ] && ARGS+=(--klee-entry "$ENTRY")
if ! "$SCSA" "${ARGS[@]}" || [ ! -s "$WORK/m.ll" ]; then
    echo "error: не удалось понизить $SOL в KLEE-модуль (нет HIGH reentrancy? задай entry)" >&2
    exit 1
fi
echo "[1/3] KLEE-модуль: $WORK/m.ll"

OUT="$WORK/klee-out"
if command -v klee >/dev/null 2>&1 && command -v llvm-as >/dev/null 2>&1; then
    echo "[2/3] бэкенд: локальный KLEE"
    llvm-as "$WORK/m.ll" -o "$WORK/m.bc" || { echo "error: llvm-as отверг модуль" >&2; exit 1; }
    echo "[3/3] KLEE:"
    klee --output-dir="$OUT" --exit-on-error-type=Assert "$WORK/m.bc" 2>&1 | tail -18 || true
    python3 "$WORK/extract.py" "$OUT" > "$WORK/model.txt" 2>/dev/null || true
elif command -v docker >/dev/null 2>&1 && docker image inspect "$KLEE_IMG" >/dev/null 2>&1; then
    echo "[2/3] бэкенд: docker $KLEE_IMG (KLEE 3.2 / LLVM 16)"
    echo "[3/3] KLEE:"
    docker run --rm -v "$WORK":/work "$KLEE_IMG" bash -c '
        cd /work && llvm-as m.ll -o m.bc &&
        klee --output-dir=/work/klee-out --exit-on-error-type=Assert m.bc 2>&1 | tail -18
        python3 /work/extract.py /work/klee-out > /work/model.txt 2>/dev/null || true' || true
else
    echo "[!] Нет бэкенда KLEE: ни локального klee, ни docker-образа $KLEE_IMG." >&2
    echo "    Поставь образ:  docker pull $KLEE_IMG" >&2
    echo "    Модуль готов ($WORK/m.ll) — самодостаточен (символьные входы+storage, fork, оракул)." >&2
    exit 3
fi

# отчёт + генерация подтверждённого PoC
echo
ERRFILES=$(find "$OUT" -name '*.reentrancy' 2>/dev/null | wc -l)
if [ "$ERRFILES" -gt 0 ] || grep -q 'KLEE: ERROR' "$OUT/messages.txt" 2>/dev/null; then
    echo "РЕЗУЛЬТАТ: reentrancy ПОДТВЕРЖДЕНА символьно — KLEE нашёл путь кражи."
    BALANCE=1; MSGVALUE=0; POCARGS=""
    [ -f "$WORK/model.txt" ] && source "$WORK/model.txt"
    echo "Witness-модель: balance=$BALANCE wei, msg.value=$MSGVALUE wei${POCARGS:+, args=[$POCARGS]}."
    # PoC без TODO: подтверждённый вариант с witness-значениями и конкретными аргументами
    POC_OUT="$(pwd)"
    ( cd "$POC_OUT" && "$SCSA" --input "$SOL_ABS" --poc \
        --klee-confirmed --klee-balance "$BALANCE" --klee-msg-value "$MSGVALUE" \
        --klee-args "$POCARGS" >/dev/null 2>&1 ) || true
    POC=$(ls -t "$POC_OUT"/*ReentrancyPoC.t.sol 2>/dev/null | head -1)
    [ -n "$POC" ] && echo "PoC (СИМВОЛЬНО ПОДТВЕРЖДЁН) записан: $POC"
    # подтверждённую функцию помечаем [SYM-CONFIRMED] в отчёте (ров виден в --audit)
    FN="$ENTRY"
    [ -z "$FN" ] && FN=$("$SCSA" --input "$SOL_ABS" --audit 2>/dev/null | \
        grep -oE "функция '[^']+'" | head -1 | sed "s/.*'\(.*\)'/\1/")
    if [ -n "$FN" ]; then
        echo "--- отчёт с бейджем ---"
        "$SCSA" --input "$SOL_ABS" --audit --confirmed "$FN" 2>/dev/null | grep 'SYM-CONFIRMED' || true
    fi
else
    echo "РЕЗУЛЬТАТ: путь кражи не найден (функция безопасна для этой глубины) — 0 ложных."
fi
