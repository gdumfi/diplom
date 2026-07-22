#!/usr/bin/env bash
#
# run_klee_corpus.sh — обобщение символьного подтверждения на корпус (K2).
# Для каждого .sol, где CFG-детектор нашёл HIGH reentrancy, понижает в KLEE-модуль
# и прогоняет KLEE (docker klee/klee, один проход по всем модулям). Считает, сколько
# находок KLEE ПОДТВЕРЖДАЕТ символически (достижим путь кражи total_sent>баланс).
#
# смысл (диплом, §8): CFG находит паттерн, KLEE отсеивает недостижимое → «precision
# символьного слоя». Non-confirm = либо реально безопасно (guard/honeypot-ловушка),
# либо ограничение модели (single-attacker, глубина 1) — честно помечаем как «не
# подтверждено», это НИЖНЯЯ оценка эксплуатируемости, а не опровержение.
#
# использование: ./benchmarks/run_klee_corpus.sh <папка-с-.sol>
# требует: docker + образ klee/klee.

set -u -o pipefail
CORPUS="${1:-}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SCSA="${SCSA_BIN:-$REPO_ROOT/build/src/solidity-llvm}"
KLEE_IMG="${KLEE_IMAGE:-klee/klee:latest}"
MAXTIME="${KLEE_MAXTIME:-20s}"

[ -z "$CORPUS" ] && { echo "usage: $0 <sol_dir>" >&2; exit 2; }
[ ! -d "$CORPUS" ] && { echo "error: нет папки $CORPUS" >&2; exit 2; }
[ ! -x "$SCSA" ] && { echo "error: собери SCSA или задай SCSA_BIN" >&2; exit 2; }
command -v docker >/dev/null 2>&1 && docker image inspect "$KLEE_IMG" >/dev/null 2>&1 || {
    echo "error: нужен docker + образ $KLEE_IMG (docker pull $KLEE_IMG)" >&2; exit 2; }

WORK="$(mktemp -d "$REPO_ROOT/build/kleecorpus.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# 1. отбираем файлы с HIGH reentrancy и понижаем в KLEE-модуль
DETECTED=0; EMIT_FAIL=0
echo "== понижение (только HIGH reentrancy) =="
for f in "$CORPUS"/*.sol; do
    name="$(basename "$f" .sol)"
    # --audit возвращает exit 2 при находках, поэтому берём вывод через подстановку
    aud="$("$SCSA" --input "$f" --audit 2>/dev/null || true)"
    echo "$aud" | grep -qiE '\[HIGH\].*reentran' || continue
    DETECTED=$((DETECTED+1))
    if "$SCSA" --input "$f" --emit-klee -o "$WORK/$name.ll" 2>/dev/null && [ -s "$WORK/$name.ll" ]; then
        :
    else
        EMIT_FAIL=$((EMIT_FAIL+1)); echo "  EMIT_FAIL $name"
    fi
done
echo "детектировано HIGH reentrancy: $DETECTED ; понижение не удалось: $EMIT_FAIL"

# 2. один docker-проход: llvm-as + klee по каждому модулю, статус в results.txt
echo "== KLEE (docker, один проход, --max-time=$MAXTIME на модуль) =="
docker run --rm -v "$WORK":/work "$KLEE_IMG" bash -c '
    MAXTIME="'"$MAXTIME"'"
    : > /work/results.txt
    for ll in /work/*.ll; do
        [ -e "$ll" ] || continue
        name="$(basename "$ll" .ll)"
        if ! llvm-as "$ll" -o "/work/$name.bc" 2>/dev/null; then
            echo "$name ASM_FAIL" >> /work/results.txt; continue
        fi
        klee --max-time="$MAXTIME" --watchdog --output-dir="/work/out.$name" \
             --exit-on-error-type=Assert "/work/$name.bc" >/dev/null 2>&1 || true
        if ls /work/out.$name/*.reentrancy >/dev/null 2>&1; then
            echo "$name CONFIRMED" >> /work/results.txt
        else
            echo "$name CLEAN" >> /work/results.txt
        fi
    done'

# 3. сводка (grep -c печатает число даже при 0 совпадений)
CONF=$(grep -c ' CONFIRMED$' "$WORK/results.txt" 2>/dev/null); CONF=${CONF:-0}
CLEAN=$(grep -c ' CLEAN$'    "$WORK/results.txt" 2>/dev/null); CLEAN=${CLEAN:-0}
ASMF=$(grep -c ' ASM_FAIL$'  "$WORK/results.txt" 2>/dev/null); ASMF=${ASMF:-0}
echo
echo "== ПОДРОБНО =="; sort "$WORK/results.txt" | sed 's/^/  /'
echo
echo "== СВОДКА (символьный слой поверх $DETECTED CFG-находок reentrancy) =="
echo "  KLEE ПОДТВЕРДИЛ (достижим путь кражи) : $CONF"
echo "  KLEE чисто (путь не достижим)          : $CLEAN"
echo "  понижение не удалось                   : $EMIT_FAIL"
echo "  сборка биткода не удалась              : $ASMF"
DEN=$((CONF+CLEAN))
[ "$DEN" -gt 0 ] && awk "BEGIN{printf \"  доля подтверждённых (из прогнанных): %.2f (%d/%d)\n\", $CONF/$DEN, $CONF, $DEN}"
