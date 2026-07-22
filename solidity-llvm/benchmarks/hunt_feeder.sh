#!/usr/bin/env bash
#
# hunt_feeder.sh — собирает адреса ВЕРИФИЦИРОВАННЫХ 0.8-контрактов mainnet для корпуса
# робастности (ROBUSTNESS-PLAN, шаг 0b). Идёт по блокам через RPC, находит создания контрактов
# (tx с to==null → receipt.contractAddress), фильтрует через Etherscan (верифицирован +
# CompilerVersion v0.8.*). Вывод — файл адресов, вход для `hunt_batch.sh --addrs`.
#
# использование: ./hunt_feeder.sh [--out FILE] [--n 20] [--offset 80000] [--blocks 300]
#   --offset — на сколько блоков от latest начать (старые блоки чаще уже верифицированы)
# требует: RPC_URL, ETHERSCAN_API_KEY, curl, python3.
set -u
OUT="corpus08.txt"; N=20; OFFSET=80000; BLOCKS=300
while [ $# -gt 0 ]; do
    case "$1" in
        --out) OUT="$2"; shift 2;;
        --n) N="$2"; shift 2;;
        --offset) OFFSET="$2"; shift 2;;
        --blocks) BLOCKS="$2"; shift 2;;
        *) echo "неизвестный аргумент: $1" >&2; exit 2;;
    esac
done
[ -z "${RPC_URL:-}" ]           && { echo "нужен RPC_URL" >&2; exit 2; }
[ -z "${ETHERSCAN_API_KEY:-}" ] && { echo "нужен ETHERSCAN_API_KEY" >&2; exit 2; }
command -v curl >/dev/null    || { echo "нужен curl" >&2; exit 2; }
command -v python3 >/dev/null || { echo "нужен python3" >&2; exit 2; }

rpc() { curl -s --max-time 25 -X POST -H 'content-type: application/json' --data "$1" "$RPC_URL"; }
hexf() { printf '0x%x' "$1"; }

latest=$(rpc '{"jsonrpc":"2.0","id":1,"method":"eth_blockNumber","params":[]}' \
         | python3 -c 'import json,sys;print(int(json.load(sys.stdin)["result"],16))' 2>/dev/null)
[ -z "$latest" ] && { echo "RPC не ответил (eth_blockNumber)" >&2; exit 3; }
start=$(( latest - OFFSET ))
echo "latest=$latest; скан с блока $start назад до $BLOCKS блоков; цель N=$N" >&2

> "$OUT"
found=0; checked=0
# ПРИМ.: RPC-эндпоинт режет ответы на 128КБ, поэтому eth_getBlockReceipts / getBlockByNumber(true)
# (полный блок) непригодны — усечение → битый JSON. Работаем на МЕЛКИХ ответах: getBlockByNumber
# (false) даёт хеши tx (мелко), пер-tx receipt тоже мелкий → берём receipt.contractAddress.
for (( b=start; b > start-BLOCKS && found < N; b-- )); do
    hashes=$(rpc "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"eth_getBlockByNumber\",\"params\":[\"$(hexf $b)\",false]}" \
             | python3 -c '
import json,sys
try: d=json.loads(sys.stdin.read(), strict=False).get("result") or {}
except: sys.exit()
for h in (d.get("transactions") or []): print(h)' 2>/dev/null)
    for h in $hashes; do
        [ $found -ge $N ] && break
        addr=$(rpc "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"eth_getTransactionReceipt\",\"params\":[\"$h\"]}" \
               | python3 -c '
import json,sys
try: r=json.loads(sys.stdin.read(), strict=False).get("result") or {}
except: print(""); sys.exit()
print(r.get("contractAddress") or "")' 2>/dev/null)
        [ -z "$addr" ] && continue        # не создание контракта
        checked=$((checked+1))
        ver=$(curl -s --max-time 20 "https://api.etherscan.io/v2/api?chainid=1&module=contract&action=getsourcecode&address=$addr&apikey=$ETHERSCAN_API_KEY" \
              | python3 -c '
import json,sys,re
try: d=json.load(sys.stdin)
except: print(""); sys.exit()
if str(d.get("status"))!="1": print(""); sys.exit()
r=d["result"][0]
if not (r.get("SourceCode") or ""): print(""); sys.exit()
m=re.search(r"v?(\d+\.\d+\.\d+)", r.get("CompilerVersion","")); print(m.group(1) if m else "")' 2>/dev/null)
        sleep 0.22
        case "$ver" in
            0.8.*) echo "$addr" >> "$OUT"; found=$((found+1)); echo "  [$found/$N] $addr ($ver)" >&2;;
        esac
    done
done
echo "проверено создания: $checked; собрано верифиц. 0.8: $found → $OUT" >&2
