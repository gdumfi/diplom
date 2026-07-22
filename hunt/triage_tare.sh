#!/bin/bash
SCA=/home/akum1/diplom/solidity-llvm/build/src/solidity-llvm
DIR=~/diplom/hunt/repos/Tare/tare-io__tare-contracts/contracts
OUT=~/diplom/hunt/tare_triage.md
echo "# Tare triage ($(date '+%H:%M'))" > $OUT
echo "" >> $OUT
n=0; hits=0
while IFS= read -r f; do
  n=$((n+1))
  rel="${f#$DIR/}"
  res=$(timeout 120 "$SCA" --input "$f" --audit --json --solc-version 0.8.33 2>/dev/null)
  if echo "$res" | grep -q '"severity"'; then
    hits=$((hits+1))
    sev=$(echo "$res" | grep -oE '"severity": "[A-Z]+"' | head -1)
    echo "## $rel — HAS FINDINGS" >> $OUT
    echo '```json' >> $OUT; echo "$res" >> $OUT; echo '```' >> $OUT
    echo "HIT: $rel"
  else
    # отличаем чисто/скип
    if echo "$res" | grep -q '\[\]'; then st="clean"; else st="skip/err"; fi
    echo "- $rel: $st" >> $OUT
  fi
done < <(find "$DIR" -name '*.sol' ! -path '*/interfaces/*' | sort)
echo "" >> $OUT
echo "итого: $n файлов, $hits с находками" >> $OUT
echo "DONE: $n files, $hits hits"
