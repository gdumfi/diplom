#!/usr/bin/env bash
#
# hunt_live.sh — ОДНА КОМАНДА для охоты. Тянет верифицированный контракт по адресу,
# сканит всеми детекторами и, если есть HIGH-находка, сразу генерит Foundry-PoC.
# Работает на Etherscan-фетче (форк/RPC не нужен).
#
#   ./hunt_live.sh 0xАДРЕС [chain]
#   chain: 1=Ethereum, 56=BSC, 137=Polygon, 42161=Arbitrum, 10=Optimism, 8453=Base
#
set -u
ADDR="${1:-}"; CHAIN="${2:-1}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCSA="$ROOT/solidity-llvm/build/src/solidity-llvm"

if [ -z "$ADDR" ]; then
    echo "как пользоваться:  ./hunt_live.sh 0xАДРЕС [chain]"
    echo "пример:            ./hunt_live.sh 0x1111111254EEB25477B68fb85Ed929f73A960582 1"
    exit 2
fi
[ ! -x "$SCSA" ] && { echo "нет бинаря SCSA ($SCSA) — собери проект"; exit 2; }

# ключ Etherscan (нужен для загрузки исходника по адресу)
[ -f "$ROOT/phase0/env.sh" ] && . "$ROOT/phase0/env.sh" >/dev/null 2>&1
if [ -z "${ETHERSCAN_API_KEY:-}" ]; then
    echo "нужен ETHERSCAN_API_KEY (бесплатный ключ на etherscan.io/apis)."
    echo "положи его в переменную окружения или в $ROOT/phase0/env.sh"
    exit 2
fi

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "🎯 цель: $ADDR  (chain $CHAIN)"
echo "1/2 · тяну исходник и сканирую…"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

JSON="$("$SCSA" --address "$ADDR" --chain "$CHAIN" --audit --json 2>/tmp/hunt_live.err)"
rc=$?

# не получилось вытянуть/собрать
if [ $rc -eq 1 ] || [ -z "$JSON" ]; then
    reason="$(grep -m1 'SCSA_SKIP:' /tmp/hunt_live.err | sed 's/.*SCSA_SKIP://')"
    echo "⚠️  не проанализировано: ${reason:-см. /tmp/hunt_live.err}"
    echo "    (частые причины: контракт не верифицирован, или это Solidity < 0.8)"
    exit 1
fi

high=$(echo "$JSON" | grep -c '"severity": "HIGH"')
med=$(echo  "$JSON" | grep -c '"severity": "MEDIUM"')
low=$(echo  "$JSON" | grep -c '"severity": "LOW"')

echo "$JSON" | python3 -c '
import json,sys
try: d=json.load(sys.stdin)
except: sys.exit()
mark={"HIGH":"[!]","MEDIUM":"[~]","LOW":"[ ]"}
for f in d:
    print("  " + mark.get(f["severity"],"·") + " " + f["severity"].ljust(6) + " " +
          f["rule"].ljust(20) + " " + f["function"] + "  (строка " + str(f["line"]) + ")")
' 2>/dev/null

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "итог: 🔴 HIGH=$high · 🟡 MED=$med · ⚪ LOW=$low"

if [ "$high" -gt 0 ]; then
    echo "2/2 · есть HIGH-кандидат — генерю PoC…"
    "$SCSA" --address "$ADDR" --chain "$CHAIN" --poc >/tmp/hunt_live_poc.log 2>&1
    poc="$(ls -t *PoC.t.sol 2>/dev/null | head -1)"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "✅ КАНДИДАТ НА БАГ. ${poc:+PoC записан: $poc}"
    echo "   дальше: покажи находку Клоду ('разбери находку по адресу $ADDR')"
    echo "   — он оценит, реальна ли, и доведёт PoC. Реально → неси на баунти."
else
    echo "чисто или мелочь (LOW/MED). Следующий адрес — этот не наш."
fi
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
