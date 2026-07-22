#!/usr/bin/env python3
# scsa_mcp.py — MCP-сервер над бинарём sca (SCSA).
#
# зачем: подписочный Claude Code получает sca как вызываемый инструмент → нейро-символическая
# петля работает без единого API-токена (Claude = LLM-аналитик, sca = детерминированный бэкенд).
#
# архитектура (AD-5): сервер — ТОНКАЯ обёртка. вся логика анализа в бинаре, тут только
# запуск процесса и парсинг вывода. никакого дублирования детекторов.
#
# протокол: MCP поверх JSON-RPC 2.0, транспорт stdio (сообщения — по строке на каждое).
# зависимости: только stdlib (как AD-1/AD-2 «ноль зависимостей»).

import json
import os
import subprocess
import sys
import urllib.request

# --- конфиг из окружения -------------------------------------------------------

# путь к бинарю sca; по умолчанию — сборка в репозитории
SCA_BIN = os.environ.get(
    "SCA_BIN", "/home/akum1/diplom/solidity-llvm/build/src/solidity-llvm"
)
# таймаут вызова бинаря (docker solc может тянуть образ) — щедрый
SCA_TIMEOUT = int(os.environ.get("SCA_TIMEOUT", "600"))

PROTOCOL_VERSION = "2025-06-18"  # если клиент просит другую — эхом вернём его версию

# публичные RPC по chain-id для sca_target_value (переопределяются env SCSA_RPC_URL_<chain>)
DEFAULT_RPC = {
    1: "https://eth.llamarpc.com",
    56: "https://bsc-dataseed.binance.org",
    137: "https://polygon-rpc.com",
    42161: "https://arb1.arbitrum.io/rpc",
    10: "https://mainnet.optimism.io",
    8453: "https://mainbase.publicnode.com",
}


def log(msg):
    # диагностика идёт в stderr, чтобы не мешать JSON-RPC в stdout
    print(f"[scsa-mcp] {msg}", file=sys.stderr, flush=True)


# --- запуск бинаря sca ---------------------------------------------------------


def run_sca(args):
    """запустить sca с args, вернуть (returncode, stdout, stderr)."""
    cmd = [SCA_BIN] + args
    try:
        p = subprocess.run(
            cmd, capture_output=True, text=True, timeout=SCA_TIMEOUT
        )
        return p.returncode, p.stdout, p.stderr
    except FileNotFoundError:
        return 127, "", f"бинарь sca не найден: {SCA_BIN} (задай env SCA_BIN)"
    except subprocess.TimeoutExpired:
        return 124, "", f"sca превысил таймаут {SCA_TIMEOUT}s"


def target_args(a):
    """собрать флаги входа: --input path ИЛИ --address 0x.. --chain N."""
    if a.get("address"):
        out = ["--address", a["address"], "--chain", str(a.get("chain", 1))]
    elif a.get("input"):
        out = ["--input", a["input"]]
    else:
        raise ValueError("нужен либо input (путь к .sol), либо address (0x...)")
    if a.get("solc_version"):
        out += ["--solc-version", str(a["solc_version"])]
    # расширение охвата: любой Blockscout/Orbit-эксплорер (L2/L3 без Etherscan-API)
    if a.get("explorer_url"):
        out += ["--explorer-url", str(a["explorer_url"])]
    return out


# --- реализация tool-ов --------------------------------------------------------


def tool_audit(a):
    """прогнать детекторы, вернуть находки, отсортированные по priorityScore."""
    args = target_args(a) + ["--audit", "--json"]
    if a.get("min_severity"):
        args += ["--min-severity", a["min_severity"]]
    rc, out, err = run_sca(args)
    try:
        findings = json.loads(out) if out.strip() else []
    except json.JSONDecodeError:
        return text_result(f"не удалось разобрать JSON от sca.\nstdout:\n{out}\nstderr:\n{err}")
    findings.sort(key=lambda f: f.get("priorityScore", 0), reverse=True)

    # деньги-приоритизация: если есть адрес — домножим на on-chain ценность
    value_note = ""
    if a.get("address"):
        val = fetch_native_value(a["address"], int(a.get("chain", 1)))
        if val is not None:
            value_note = f"\non-chain native баланс цели ≈ {val:.4f} (приоритизируй дорогие цели)"

    summary = {
        "target": a.get("address") or a.get("input"),
        "count": len(findings),
        "findings": findings,
    }
    hdr = f"SCSA audit: {summary['count']} находок (отсортированы по приоритету).{value_note}\n\n"
    return text_result(hdr + json.dumps(summary, ensure_ascii=False, indent=2))


def tool_context_pack(a):
    """markdown-сенсор для LLM-петли: IR-факты + гипотезы + исходник."""
    args = target_args(a) + ["--llm-context"]
    rc, out, err = run_sca(args)
    if not out.strip():
        return text_result(f"пустой контекст-пак.\nstderr:\n{err}")
    return text_result(out)


def tool_poc(a):
    """сгенерировать Foundry-PoC. форк-режим (--victim/--fork-block) = истина на реальном EVM."""
    args = target_args(a) + ["--poc"]
    if a.get("victim"):
        args += ["--victim", a["victim"]]
    if a.get("fork_block"):
        args += ["--fork-block", str(a["fork_block"])]
    if a.get("klee_confirmed"):
        args += ["--klee-confirmed"]
    rc, out, err = run_sca(args)
    body = out if out.strip() else err
    return text_result(f"# SCSA PoC (rc={rc})\n\n{body}")


def tool_invariant(a):
    """forge stateful-invariant харнесс; --victim → форк против живого контракта."""
    prop = a.get("property", "solvency")
    args = target_args(a) + ["--emit-invariant", "--invariant-property", prop]
    if a.get("victim"):
        args += ["--victim", a["victim"]]
    if a.get("fork_block"):
        args += ["--fork-block", str(a["fork_block"])]
    rc, out, err = run_sca(args)
    body = out if out.strip() else err
    return text_result(f"# SCSA invariant harness ({prop}, rc={rc})\n\n{body}")


def tool_target_value(a):
    """on-chain ценность цели (native + опц. токены) — приоритизация по деньгам."""
    addr = a["address"]
    chain = int(a.get("chain", 1))
    val = fetch_native_value(addr, chain)
    if val is None:
        return text_result(f"не удалось получить баланс {addr} (chain {chain}). "
                           f"Задай env SCSA_RPC_URL_{chain}.")
    return text_result(json.dumps(
        {"address": addr, "chain": chain, "native_balance": val,
         "note": "ценность эксплойта коррелирует с активами контракта (SCONE-bench)"},
        ensure_ascii=False, indent=2))


def fetch_native_value(addr, chain):
    """eth_getBalance через RPC; вернуть баланс в единицах (float) или None."""
    rpc = os.environ.get(f"SCSA_RPC_URL_{chain}") or DEFAULT_RPC.get(chain)
    if not rpc:
        return None
    payload = json.dumps({
        "jsonrpc": "2.0", "id": 1, "method": "eth_getBalance",
        "params": [addr, "latest"],
    }).encode()
    try:
        req = urllib.request.Request(
            rpc, data=payload, headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=15) as r:
            res = json.loads(r.read())
        wei = int(res["result"], 16)
        return wei / 1e18
    except Exception as e:  # noqa: BLE001 — сеть ненадёжна, деградируем мягко
        log(f"RPC fetch failed: {e}")
        return None


# --- реестр tool-ов (схемы для tools/list) -------------------------------------

TARGET_PROPS = {
    "input": {"type": "string", "description": "путь к .sol файлу"},
    "address": {"type": "string", "description": "адрес контракта 0x... (вместо input)"},
    "chain": {"type": "integer", "description": "chain id для address (1=mainnet)", "default": 1},
    "solc_version": {"type": "string", "description": "тег образа solc (напр. 0.8.19)"},
    "explorer_url": {"type": "string", "description": "Blockscout/Orbit explorer API base (https://<host>/api) для L2/L3 без Etherscan (напр. Intuition)"},
}

TOOLS = [
    {
        "name": "sca_audit",
        "description": "Запустить детекторы безопасности SCSA (reentrancy, access-control, oracle, "
                       "slippage, arbitrary-call и др.) на .sol-файле или верифицированном контракте "
                       "по адресу. Возвращает находки, отсортированные по денежному приоритету. "
                       "Первый шаг нейро-символической петли.",
        "inputSchema": {
            "type": "object",
            "properties": {**TARGET_PROPS,
                           "min_severity": {"type": "string", "enum": ["HIGH", "MEDIUM", "LOW"]}},
        },
        "_fn": tool_audit,
    },
    {
        "name": "sca_context_pack",
        "description": "Собрать neuro-symbolic контекст-пак (--llm-context): факты из IR (storage-слоты, "
                       "внешние вызовы, owner-guard, oracle, mapping), зацепки детекторов, гипотезы под "
                       "SWC/Solodit и рекомендованные инварианты + исходник. Это СЕНСОР для анализа: "
                       "прочитай его, выдвини гипотезу об эксплойте, затем подтверди через sca_invariant/sca_poc.",
        "inputSchema": {"type": "object", "properties": TARGET_PROPS},
        "_fn": tool_context_pack,
    },
    {
        "name": "sca_poc",
        "description": "Сгенерировать Foundry-PoC эксплойт для находки. С victim+fork_block — форк-режим: "
                       "эксплойт против РЕАЛЬНО развёрнутого контракта (это истина на настоящем EVM).",
        "inputSchema": {
            "type": "object",
            "properties": {**TARGET_PROPS,
                           "victim": {"type": "string", "description": "адрес живой жертвы 0x... (форк-режим)"},
                           "fork_block": {"type": "integer", "description": "блок форка (до патча)"},
                           "klee_confirmed": {"type": "boolean", "description": "пометить как символьно подтверждённый"}},
        },
        "_fn": tool_poc,
    },
    {
        "name": "sca_invariant",
        "description": "Сгенерировать Foundry stateful-invariant харнесс (property: solvency|inflation|"
                       "inflation-erc20|inflation-4626|roundtrip|supply). Ломается на уязвимом, держится на "
                       "безопасном. С victim → форк против живого контракта = подтверждение на реальном EVM.",
        "inputSchema": {
            "type": "object",
            "properties": {**TARGET_PROPS,
                           "property": {"type": "string",
                                        "enum": ["solvency", "inflation", "inflation-erc20",
                                                 "inflation-4626", "roundtrip", "supply"]},
                           "victim": {"type": "string"},
                           "fork_block": {"type": "integer"}},
        },
        "_fn": tool_invariant,
    },
    {
        "name": "sca_target_value",
        "description": "Получить on-chain ценность контракта (native баланс) для приоритизации по деньгам. "
                       "Ценность эксплойта коррелирует с активами контракта — хантить дорогие цели первыми.",
        "inputSchema": {
            "type": "object",
            "properties": {"address": {"type": "string"}, "chain": {"type": "integer", "default": 1}},
            "required": ["address"],
        },
        "_fn": tool_target_value,
    },
]

TOOL_BY_NAME = {t["name"]: t for t in TOOLS}


def public_tools():
    # схемы без внутреннего _fn
    return [{k: v for k, v in t.items() if not k.startswith("_")} for t in TOOLS]


def text_result(s):
    return {"content": [{"type": "text", "text": s}]}


# --- JSON-RPC цикл -------------------------------------------------------------


def handle(req):
    """обработать один JSON-RPC запрос, вернуть ответ (или None для нотификаций)."""
    method = req.get("method")
    rid = req.get("id")

    if method == "initialize":
        client_ver = (req.get("params") or {}).get("protocolVersion", PROTOCOL_VERSION)
        return ok(rid, {
            "protocolVersion": client_ver,
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "scsa", "version": "0.1.0"},
        })

    if method == "notifications/initialized":
        return None  # нотификация — без ответа

    if method == "ping":
        return ok(rid, {})

    if method == "tools/list":
        return ok(rid, {"tools": public_tools()})

    if method == "tools/call":
        params = req.get("params") or {}
        name = params.get("name")
        args = params.get("arguments") or {}
        tool = TOOL_BY_NAME.get(name)
        if not tool:
            return err(rid, -32602, f"неизвестный tool: {name}")
        try:
            result = tool["_fn"](args)
            return ok(rid, result)
        except Exception as e:  # noqa: BLE001 — вернём ошибку как tool-результат
            return ok(rid, text_result(f"ошибка выполнения {name}: {e}"))

    # неизвестный метод
    if rid is not None:
        return err(rid, -32601, f"метод не поддержан: {method}")
    return None


def ok(rid, result):
    return {"jsonrpc": "2.0", "id": rid, "result": result}


def err(rid, code, message):
    return {"jsonrpc": "2.0", "id": rid, "error": {"code": code, "message": message}}


def main():
    log(f"старт, SCA_BIN={SCA_BIN}")
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except json.JSONDecodeError:
            continue
        # поддержка батчей (список) и одиночных запросов
        reqs = req if isinstance(req, list) else [req]
        for r in reqs:
            resp = handle(r)
            if resp is not None:
                sys.stdout.write(json.dumps(resp, ensure_ascii=False) + "\n")
                sys.stdout.flush()


if __name__ == "__main__":
    main()
