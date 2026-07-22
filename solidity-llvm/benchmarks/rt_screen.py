#!/usr/bin/env python3
# Быстрый roundtrip-скрин ERC4626 по адресу против ЖИВОГО mainnet (без Etherscan/платного ключа).
# Свойство: convertToAssets(convertToShares(x)) <= x должно держаться (округление не создаёт стоимость).
# КРИТИЧНО: обе операции — В ОДНОМ зафиксированном блоке. Иначе yield-начисление между двумя
# eth_call на "latest" поднимает share-цену → мнимое back>x (~1e-8) → массовые FALSE POSITIVE
# на аудированных vault-ах (sUSDe/sUSDS/Morpho/…). Это НЕ баг, а дрейф между блоками.
# break на ПИН-блоке = реальный кандидат (rounding-creates-value). Дальше — forge-fork подтверждение.
#
# usage: python3 rt_screen.py <addr1> <addr2> ...   (или отредактируй VAULTS ниже)
import urllib.request, json, sys

UA = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36"  # публичные RPC блокируют дефолтный UA urllib
RPCS = ["https://ethereum.publicnode.com", "https://1rpc.io/eth"]
CTS, CTA = "0xc6e6f592", "0x07a2d13a"   # convertToShares(uint256) / convertToAssets(uint256)

def rpc(method, params):
    for r in RPCS:
        try:
            req = urllib.request.Request(r, data=json.dumps(
                {"jsonrpc":"2.0","method":method,"params":params,"id":1}).encode(),
                headers={"Content-Type":"application/json","User-Agent":UA})
            return json.load(urllib.request.urlopen(req, timeout=20)).get("result")
        except Exception:
            continue
    return None

def ethcall(to, data, block):
    res = rpc("eth_call", [{"to":to,"data":data}, block])
    return int(res[:66],16) if res and res != "0x" else None

# несколько x, включая odd/крупные — где округление флипается
XS = [3, 999999999, 10**18, 999999999999999999, 314159265358979323, 10**27+7]

def screen(name, addr, block):
    if ethcall(addr, CTS+f"{10**18:064x}", block) is None:
        return (name, "NO", None)                    # не-ERC4626 / неверный адрес
    for x in XS:
        s = ethcall(addr, CTS+f"{x:064x}", block)
        if s is None: continue
        b = ethcall(addr, CTA+f"{s:064x}", block)    # ТОТ ЖЕ block
        if b is not None and b > x:
            return (name, "BREAK", (x, s, b))         # кандидат: округление создаёт стоимость
    return (name, "ok", None)

VAULTS = {}  # {name: addr} — заполни или передай адреса аргументами

if __name__ == "__main__":
    block = rpc("eth_blockNumber", [])               # ПИН: один блок на весь скрин
    print("pinned block:", int(block,16) if block else "?")
    items = VAULTS.items() if not sys.argv[1:] else [(a, a) for a in sys.argv[1:]]
    for name, addr in items:
        n, verdict, det = screen(name, addr, block)
        print(f"{n:24} {verdict:6} {det if det else ''}", flush=True)
