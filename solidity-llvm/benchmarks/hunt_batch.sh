#!/usr/bin/env bash
#
# hunt_batch.sh — Фаза C: ДЕШЁВАЯ ШИРОКАЯ ВОРОНКА. Гоняет детерминированный `sca --audit`
# по многим контрактам (локальный каталог .sol ИЛИ список адресов Etherscan), классифицирует
# результат и строит ТРИАЖ-ОЧЕРЕДЬ выживших (есть HIGH) для глубокого разбора через `/hunt`.
# Claude на этом этапе НЕ участвует — только бинарь (экономит лимиты подписки; см. воронку
# в docs/HUNT-PLAYBOOK.md §1). Выжившие идут дальше в --llm-context → /hunt → доказательство.
#
# использование:
#   ./hunt_batch.sh --dir   <каталог с .sol>
#   ./hunt_batch.sh --addrs <файл: строки вида 0xADDRESS[,chain]>   (нужен ETHERSCAN_API_KEY)
# опц.: --out <файл> (по умолчанию hunt_queue.md), --limit N (первые N целей), --chain N (дефолт 1)
#
# вывод: hunt_queue.md — очередь с приоритетом; на stdout — сводка.
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SCSA="${SCSA_BIN:-$REPO_ROOT/build/src/solidity-llvm}"

MODE=""; SRC=""; OUT="hunt_queue.md"; LIMIT=0; CHAIN=1
while [ $# -gt 0 ]; do
    case "$1" in
        --dir)   MODE="dir";   SRC="$2"; shift 2;;
        --addrs) MODE="addrs"; SRC="$2"; shift 2;;
        --out)   OUT="$2"; shift 2;;
        --limit) LIMIT="$2"; shift 2;;
        --chain) CHAIN="$2"; shift 2;;
        *) echo "неизвестный аргумент: $1" >&2; exit 2;;
    esac
done
[ -z "$MODE" ]   && { echo "usage: $0 --dir <dir> | --addrs <file> [--limit N] [--out F] [--chain N]" >&2; exit 2; }
[ ! -x "$SCSA" ] && { echo "error: собери SCSA или задай SCSA_BIN" >&2; exit 2; }

# собираем список целей: "input_desc<TAB>args_for_sca"
TARGETS=()
if [ "$MODE" = "dir" ]; then
    [ ! -d "$SRC" ] && { echo "error: нет каталога $SRC" >&2; exit 2; }
    while IFS= read -r f; do TARGETS+=("$f"$'\t'"--input|$f"); done \
        < <(find "$SRC" -name '*.sol' | sort)
else
    [ ! -f "$SRC" ] && { echo "error: нет файла $SRC" >&2; exit 2; }
    [ -z "${ETHERSCAN_API_KEY:-}" ] && echo "warn: ETHERSCAN_API_KEY не задан — фетч упадёт" >&2
    while IFS= read -r line; do
        line="${line%%#*}"; line="$(echo "$line" | tr -d '[:space:]')"; [ -z "$line" ] && continue
        addr="${line%%,*}"; ch="${line#*,}"; [ "$ch" = "$line" ] && ch="$CHAIN"
        TARGETS+=("$addr (chain $ch)"$'\t'"--address|$addr|--chain|$ch"); done < "$SRC"
fi
[ "${#TARGETS[@]}" -eq 0 ] && { echo "нет целей." >&2; exit 1; }
[ "$LIMIT" -gt 0 ] && TARGETS=("${TARGETS[@]:0:$LIMIT}")

echo "воронка: ${#TARGETS[@]} целей (режим $MODE)"

# накопители строк по категориям
SURV=""; MINOR=""; CLEAN=""; SKIP=""
declare -A SKIPCNT          # распределение SKIP по коду причины (ROBUSTNESS-PLAN §2)
n=0
for t in "${TARGETS[@]}"; do
    desc="${t%%$'\t'*}"; rawargs="${t#*$'\t'}"
    IFS='|' read -r -a A <<< "$rawargs"
    n=$((n+1)); printf '[%d/%d] %s ... ' "$n" "${#TARGETS[@]}" "$desc"
    ERRF="$(mktemp)"
    JSON="$(timeout 120 "$SCSA" "${A[@]}" --audit --json 2>"$ERRF")"; rc=$?
    if [ $rc -eq 1 ] || [ -z "$JSON" ]; then
        # бакетим по структурному коду SCSA_SKIP (A0/A1/A2/A3/B1/C), выданному бинарём
        code="$(grep -m1 -oE 'SCSA_SKIP:[A-Z0-9]+' "$ERRF" | head -1 | cut -d: -f2)"
        [ -z "$code" ] && code="UNK"
        detail="$(grep -m1 'SCSA_SKIP:' "$ERRF" | sed -E 's/.*SCSA_SKIP:[A-Z0-9]+ ?//' | head -c 50)"
        rm -f "$ERRF"
        SKIPCNT[$code]=$(( ${SKIPCNT[$code]:-0} + 1 ))
        echo "SKIP [$code] $detail"; SKIP+="| $desc | $code | $detail |"$'\n';
        [ "$MODE" = "addrs" ] && sleep 0.3; continue
    fi
    rm -f "$ERRF"
    if [ "$JSON" = "[]" ] || ! echo "$JSON" | grep -q '"severity"'; then
        echo "clean"; CLEAN+="| $desc |"$'\n'; [ "$MODE" = "addrs" ] && sleep 0.3; continue
    fi
    high=$(echo "$JSON" | grep -c '"severity": "HIGH"')
    med=$(echo  "$JSON" | grep -c '"severity": "MEDIUM"')
    low=$(echo  "$JSON" | grep -c '"severity": "LOW"')
    rules=$(echo "$JSON" | grep -oE '"rule": "[^"]+"' | sed 's/"rule": //; s/"//g' | sort | uniq -c \
            | awk '{printf "%s×%s ", $2, $1}')
    # команда для глубокого разбора
    if [ "$MODE" = "dir" ]; then cmd="/hunt ${A[1]}"; else cmd="/hunt ${A[1]} ${A[3]}"; fi
    row="| $desc | $high | $med | $low | ${rules}| \`$cmd\` |"
    if [ "$high" -gt 0 ]; then echo "★ SURVIVOR (HIGH=$high)"; SURV+="$row"$'\n';
    else echo "minor (M=$med L=$low)"; MINOR+="$row"$'\n'; fi
    [ "$MODE" = "addrs" ] && sleep 0.3
done

# --- пишем очередь ---
{
  echo "# HUNT QUEUE — триаж воронки \`sca --audit\`"
  echo
  echo "> Сгенерировано $(date '+%Y-%m-%d %H:%M') · режим \`$MODE\` · источник \`$SRC\` · целей ${#TARGETS[@]}."
  echo "> Детерминированный слой (без Claude). Выживших — разбирать через \`/hunt\` (дорогой слой)."
  echo
  echo "## ★ Выжившие — есть HIGH (приоритет разбора)"
  echo
  if [ -n "$SURV" ]; then
    echo "| цель | HIGH | MED | LOW | классы | разбор |"; echo "|---|---|---|---|---|---|"; printf '%s' "$SURV"
  else echo "_нет — ни одного HIGH в этой партии._"; fi
  echo
  echo "## Второстепенные — только MEDIUM/LOW"
  echo
  if [ -n "$MINOR" ]; then
    echo "| цель | HIGH | MED | LOW | классы | разбор |"; echo "|---|---|---|---|---|---|"; printf '%s' "$MINOR"
  else echo "_нет._"; fi
  echo
  echo "## Чисто (детекторы молчат)"; echo
  [ -n "$CLEAN" ] && { echo "| цель |"; echo "|---|"; printf '%s' "$CLEAN"; } || echo "_нет._"
  echo
  echo "## Пропущены (не собрались / не получены)"; echo
  echo "Коды (ROBUSTNESS-PLAN §2): A0 фетч · A1 нет образа solc · A2 не та версия ·"
  echo "A3 ошибка компиляции · B1 сбой лоуэринга · C pre-0.8 (вне охвата) · UNK неопознан."
  echo
  if [ -n "${SKIPCNT[*]+x}" ]; then
    echo "**Распределение SKIP по причине (baseline-замер):**"; echo
    echo "| код | кол-во |"; echo "|---|---|"
    for c in "${!SKIPCNT[@]}"; do echo "| $c | ${SKIPCNT[$c]} |"; done | sort
    echo
  fi
  [ -n "$SKIP" ] && { echo "| цель | код | деталь |"; echo "|---|---|---|"; printf '%s' "$SKIP"; } || echo "_нет пропусков._"
} > "$OUT"

nsurv=$(printf '%s' "$SURV" | grep -c '|' || true)
echo
echo "готово → $OUT"
echo "выживших с HIGH: $nsurv (их — в /hunt)"
if [ -n "${SKIPCNT[*]+x}" ]; then
    echo "SKIP по причине:"
    for c in "${!SKIPCNT[@]}"; do echo "  $c: ${SKIPCNT[$c]}"; done | sort
fi
