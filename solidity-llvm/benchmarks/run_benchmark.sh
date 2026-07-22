#!/usr/bin/env bash
#
# run_benchmark.sh — прогон детектора reentrancy по корпусу .sol и подсчёт
# precision / recall / F1 относительно ручных меток (csv).
#
# честная методология: subset компилятора ограничен (только uint256/bool/address,
# один контракт на файл, без mapping/наследования/событий/структур/строк). многие
# контракты SmartBugs НЕ компилируются нашим фронтендом — такие НЕ входят в метрику,
# они выносятся в отдельную категорию "не скомпилировалось" с причиной. мы НЕ
# засчитываем нескомпилированный файл ни как покрытый, ни как пропуск детектора.
#
# использование:
#   ./benchmarks/run_benchmark.sh <папка-с-.sol> <labels.csv> [путь-к-бинарю]
#
# формат labels.csv (см. labels.example.csv):
#   filename,expected_vulnerable
#   reentrancy_dao.sol,1
#   safe_vault.sol,0
# где expected_vulnerable: 1 = файл уязвим (reentrancy), 0 = безопасен.
# сопоставление идёт по базовому имени файла (basename), регистр учитывается.

set -u -o pipefail

# ---------------------------------------------------------------------------
# аргументы и значения по умолчанию
# ---------------------------------------------------------------------------
SOL_DIR="${1:-}"
LABELS_CSV="${2:-}"
# бинарь: третий аргумент, либо переменная окружения, либо путь по умолчанию.
# путь считаем от корня репозитория (родитель папки скрипта).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN="${3:-${SCSA_BIN:-$REPO_ROOT/build/src/solidity-llvm}}"

usage() {
    cat >&2 <<EOF
usage: $0 <sol_dir> <labels.csv> [binary]

  sol_dir      папка с *.sol файлами корпуса
  labels.csv   csv с колонками filename,expected_vulnerable (1/0)
  binary       путь к solidity-llvm (по умолчанию $REPO_ROOT/build/src/solidity-llvm,
               либо переменная окружения SCSA_BIN)

пример:
  $0 ./corpus/reentrancy ./benchmarks/labels.csv
EOF
    exit 2
}

[ -z "$SOL_DIR" ] && usage
[ -z "$LABELS_CSV" ] && usage

if [ ! -d "$SOL_DIR" ]; then
    echo "error: папка с корпусом не найдена: $SOL_DIR" >&2
    exit 2
fi
if [ ! -f "$LABELS_CSV" ]; then
    echo "error: файл меток не найден: $LABELS_CSV" >&2
    exit 2
fi
if [ ! -x "$BIN" ]; then
    echo "error: бинарь не найден или не исполняем: $BIN" >&2
    echo "       собери проект или укажи путь третьим аргументом / через SCSA_BIN" >&2
    exit 2
fi

# LLVM 18 в PATH нужен, если бинарь линкуется динамически (на всякий случай)
export PATH="/usr/lib/llvm-18/bin:$PATH"

# ---------------------------------------------------------------------------
# счётчики
# ---------------------------------------------------------------------------
TP=0    # уязвим по метке И детектор нашёл
FP=0    # безопасен по метке НО детектор нашёл
FN=0    # уязвим по метке НО детектор промолчал
TN=0    # безопасен по метке И детектор промолчал
NOCOMPILE=0   # не скомпилировалось (вне метрики)
MISSING=0     # файл из меток отсутствует на диске (вне метрики)

# журнал по каждому файлу (для итоговой таблицы)
declare -a LOG_LINES=()

# ---------------------------------------------------------------------------
# запуск детектора над одним файлом.
# печатает в stdout ОДНУ строку вида "КЛАСС<TAB>причина":
#   VULN     — скомпилировалось, детектор дал находку(и)
#   CLEAN    — скомпилировалось, находок нет
#   NOCOMPILE— фронтенд не осилил файл (лекс/парс/сема/кодоген ошибка) + причина
# вызывается через $(...) (субшелл), поэтому причину возвращаем через stdout,
# а не через глобальную переменную — из субшелла она бы не пробросилась.
# ---------------------------------------------------------------------------
run_one() {
    local f="$1"
    local REASON=""
    local out rc

    # пытаемся json-режим (появится в будущем). сейчас бинарь его не знает и
    # падает с "Option 'json' does not exist" — тогда откатываемся на консольный.
    out="$("$BIN" --input "$f" --audit --json 2>&1)"
    rc=$?
    if printf '%s' "$out" | grep -q "Option 'json' does not exist"; then
        # json не поддержан — консольный режим (стабильный контракт по кодам возврата)
        out="$("$BIN" --input "$f" --audit 2>&1)"
        rc=$?
    fi

    # интерпретация:
    #   rc==1              -> NOCOMPILE (фронтенд не осилил файл)
    #   вывод содержит HIGH -> VULN  (детектор говорит «это баг»)
    #   иначе (нет находок  -> CLEAN
    #     или только LOW [guarded])
    # ВАЖНО: severity LOW/[guarded] — это НЕ вердикт «уязвим», а «есть guard, вероятно
    # ок». поэтому в метрике «уязвим» = наличие HIGH, а не сам факт находки (exit 2
    # выставляется на ЛЮБУЮ находку, включая LOW, и не годится как критерий).
    if [ "$rc" = "1" ]; then
        REASON="$(printf '%s' "$out" | grep -m1 -i 'error:' | sed 's/^[[:space:]]*//')"
        [ -z "$REASON" ] && REASON="компиляция не удалась (код 1), вывод: $(printf '%s' "$out" | head -c 120 | tr '\n' ' ')"
        printf 'NOCOMPILE\t%s\n' "$REASON"
        return
    fi

    # HIGH ловим и в json ("severity": "HIGH"), и в консоли ([HIGH])
    if printf '%s' "$out" | grep -Eq '"severity"[[:space:]]*:[[:space:]]*"HIGH"|\[HIGH\]'; then
        printf 'VULN\t\n'
    else
        printf 'CLEAN\t\n'
    fi
}

# ---------------------------------------------------------------------------
# основной цикл по строкам меток
# ---------------------------------------------------------------------------
echo "SCSA reentrancy benchmark"
echo "  корпус : $SOL_DIR"
echo "  метки  : $LABELS_CSV"
echo "  бинарь : $BIN"
echo

# читаем csv; пропускаем пустые строки, комментарии (#) и строку-заголовок.
lineno=0
while IFS=',' read -r fname expected rest || [ -n "$fname" ]; do
    lineno=$((lineno + 1))

    # чистим пробелы/CR (файлы из-под windows)
    fname="$(printf '%s' "$fname" | tr -d '\r' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"
    expected="$(printf '%s' "$expected" | tr -d '\r' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"

    # пропуск пустых и комментариев
    [ -z "$fname" ] && continue
    case "$fname" in \#*) continue ;; esac

    # пропуск заголовка (регистронезависимо)
    lc="$(printf '%s' "$fname" | tr '[:upper:]' '[:lower:]')"
    if [ "$lc" = "filename" ] || [ "$lc" = "file" ]; then
        continue
    fi

    # валидация метки
    if [ "$expected" != "0" ] && [ "$expected" != "1" ]; then
        echo "warn: строка $lineno: неверная метка '$expected' для '$fname' (ожидалось 0/1) — пропуск" >&2
        continue
    fi

    # ищем файл: сначала как указано, затем по basename внутри SOL_DIR
    path=""
    if [ -f "$SOL_DIR/$fname" ]; then
        path="$SOL_DIR/$fname"
    elif [ -f "$fname" ]; then
        path="$fname"
    else
        # поиск по basename (метки могут содержать подпапки)
        base="$(basename "$fname")"
        found="$(find "$SOL_DIR" -type f -name "$base" 2>/dev/null | head -n1)"
        [ -n "$found" ] && path="$found"
    fi

    if [ -z "$path" ]; then
        MISSING=$((MISSING + 1))
        LOG_LINES+=("MISSING   | exp=$expected | $fname | файл не найден в корпусе")
        continue
    fi

    # run_one печатает "КЛАСС<TAB>причина"; разбираем по табу
    result="$(run_one "$path")"
    class="${result%%$'\t'*}"
    reason="${result#*$'\t'}"

    if [ "$class" = "NOCOMPILE" ]; then
        NOCOMPILE=$((NOCOMPILE + 1))
        LOG_LINES+=("NOCOMPILE | exp=$expected | $fname | $reason")
        continue
    fi

    # class == VULN | CLEAN, метка == 1 | 0
    if [ "$expected" = "1" ] && [ "$class" = "VULN" ]; then
        TP=$((TP + 1)); LOG_LINES+=("TP        | exp=1 | $fname | детектор: уязвим")
    elif [ "$expected" = "0" ] && [ "$class" = "VULN" ]; then
        FP=$((FP + 1)); LOG_LINES+=("FP        | exp=0 | $fname | детектор: уязвим (ложное срабатывание)")
    elif [ "$expected" = "1" ] && [ "$class" = "CLEAN" ]; then
        FN=$((FN + 1)); LOG_LINES+=("FN        | exp=1 | $fname | детектор: чисто (пропуск)")
    else
        TN=$((TN + 1)); LOG_LINES+=("TN        | exp=0 | $fname | детектор: чисто")
    fi
done < "$LABELS_CSV"

# ---------------------------------------------------------------------------
# журнал по файлам
# ---------------------------------------------------------------------------
echo "--- по файлам ---"
for l in "${LOG_LINES[@]}"; do
    echo "  $l"
done
echo

# ---------------------------------------------------------------------------
# метрики. считаем через awk (bash не умеет float).
# precision = TP / (TP + FP)
# recall    = TP / (TP + FN)
# F1        = 2*P*R / (P + R)
# граничные случаи (деление на 0) отдаём как n/a.
# ---------------------------------------------------------------------------
EVAL_TOTAL=$((TP + FP + FN + TN))

echo "--- сводка ---"
echo "  TP (уязвим, найден)        : $TP"
echo "  FP (безопасен, найден)     : $FP"
echo "  FN (уязвим, пропущен)      : $FN"
echo "  TN (безопасен, чисто)      : $TN"
echo "  в метрике всего            : $EVAL_TOTAL"
echo "  не скомпилировалось (вне)  : $NOCOMPILE"
echo "  нет файла (вне)            : $MISSING"
echo

if [ "$EVAL_TOTAL" -eq 0 ]; then
    echo "  метрики: n/a — ни один файл не попал в метрику (всё вне: nocompile/missing)."
    echo
    echo "  предупреждение: 0 оцениваемых файлов. проверь, что корпус приведён к subset"
    echo "  (см. README.md, раздел ограничения)."
    exit 0
fi

read -r PREC REC F1 <<EOF
$(awk -v tp="$TP" -v fp="$FP" -v fn="$FN" 'BEGIN{
    if (tp+fp>0) p=tp/(tp+fp); else p=-1;
    if (tp+fn>0) r=tp/(tp+fn); else r=-1;
    if (p>=0 && r>=0 && (p+r)>0) f=2*p*r/(p+r); else f=-1;
    printf "%s %s %s", (p<0?"n/a":sprintf("%.4f",p)), (r<0?"n/a":sprintf("%.4f",r)), (f<0?"n/a":sprintf("%.4f",f));
}')
EOF

echo "  precision : $PREC"
echo "  recall    : $REC"
echo "  F1        : $F1"
echo

# машиночитаемая строка (удобно грепать/собирать в таблицу)
echo "RESULT tp=$TP fp=$FP fn=$FN tn=$TN nocompile=$NOCOMPILE missing=$MISSING precision=$PREC recall=$REC f1=$F1"
