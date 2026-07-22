#!/usr/bin/env bash
# прогон курируемого 0.8-корпуса через solc-фронтенд (дефолт). сверка с меткой `// expect:`.
# метрика: TP/FP/FN по HIGH reentrancy|access-control на уровне файла.
set -u
BIN="${BIN:-$(dirname "$0")/../build/src/solidity-llvm}"
DIR="$(dirname "$0")/corpus08"

TP=0; FP=0; FN=0; TN=0
printf "%-28s %-16s %-16s %s\n" "файл" "ожидалось" "найдено(HIGH)" "итог"
for f in "$DIR"/*.sol; do
    expect=$(grep -oE 'expect: *(read-only-reentrancy|reentrancy|access-control|arbitrary-call|weak-randomness|price-manipulation|selfdestruct|delegatecall|tx-origin|none)' "$f" | head -1 | sed 's/expect: *//')
    out=$("$BIN" --input "$f" --audit 2>/dev/null || true)
    got=""
    # учитываем HIGH/MEDIUM классы (LOW unchecked-call — информационный, не в счёт)
    for rule in read-only-reentrancy reentrancy access-control arbitrary-call weak-randomness price-manipulation selfdestruct delegatecall tx-origin; do
        echo "$out" | grep -qE "\[(HIGH|MEDIUM)\] $rule" && got="$got $rule"
    done
    got=$(echo "$got" | xargs)   # trim
    [ -z "$got" ] && got="none"

    verdict="OK"
    if [ "$expect" = "none" ]; then
        if [ "$got" = "none" ]; then TN=$((TN+1)); else FP=$((FP+1)); verdict="FP!"; fi
    else
        if echo "$got" | grep -qw "$expect"; then TP=$((TP+1)); else FN=$((FN+1)); verdict="FN!"; fi
        # лишний класс на vuln-файле сверх ожидаемого — тоже ложное срабатывание
        for g in $got; do [ "$g" != "$expect" ] && [ "$g" != "none" ] && { FP=$((FP+1)); verdict="$verdict +FP($g)"; }; done
    fi
    printf "%-28s %-16s %-16s %s\n" "$(basename "$f")" "$expect" "$got" "$verdict"
done

echo "----------------------------------------------------------------"
echo "TP=$TP  FP=$FP  FN=$FN  TN=$TN"
prec=$(awk "BEGIN{ d=$TP+$FP; printf (d? \"%.3f\":\"n/a\"), (d? $TP/d:0) }")
rec=$(awk  "BEGIN{ d=$TP+$FN; printf (d? \"%.3f\":\"n/a\"), (d? $TP/d:0) }")
echo "Precision=$prec  Recall=$rec"
