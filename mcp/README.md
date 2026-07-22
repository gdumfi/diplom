# SCSA MCP-сервер

Делает бинарь `sca` вызываемым инструментом для подписочного Claude Code — **нейро-символическая
петля без единого API-токена**: Claude = LLM-аналитик, `sca` = детерминированный символьный бэкенд.

Тонкая обёртка (AD-5): сервер только запускает бинарь и парсит вывод. Логики анализа тут нет.
Stdlib-only Python, MCP поверх JSON-RPC 2.0 по stdio.

## Регистрация

Уже прописан в `~/diplom/.mcp.json` (сервер `scsa`). Claude Code подхватит его автоматически в
этом проекте. Проверить: `/mcp` в интерактивной сессии → должен быть `scsa` с 5 tool-ами.

Глобально (в любом проекте): `claude mcp add scsa python3 /home/akum1/diplom/mcp/scsa_mcp.py`.

## Инструменты

| tool | что делает | под капотом |
|---|---|---|
| `sca_audit` | детекторы → находки по денежному приоритету | `--audit --json` |
| `sca_context_pack` | markdown-сенсор (IR-факты + гипотезы) — вход в петлю | `--llm-context` |
| `sca_poc` | Foundry-PoC; `victim`+`fork_block` = истина на реальном EVM | `--poc [--victim --fork-block]` |
| `sca_invariant` | forge stateful-invariant харнесс | `--emit-invariant --invariant-property` |
| `sca_target_value` | on-chain баланс цели — хантить дорогие первыми | RPC `eth_getBalance` |

Вход у большинства: `input` (путь к .sol) ИЛИ `address`+`chain` (верифиц. контракт с Etherscan).

## Петля хантинга (пример для Claude)

1. `sca_audit {address, chain}` → находки, отсортированы по деньгам.
2. `sca_context_pack {address}` → прочитать IR-факты, выдвинуть гипотезу об эксплойте.
3. `sca_invariant {input, property}` или `sca_poc {input, victim, fork_block}` → подтвердить на форке.
4. forge-зелёный = доказанный баг → сабмит.

## Env

- `SCA_BIN` — путь к бинарю (по умолчанию сборка в репо).
- `SCA_TIMEOUT` — таймаут вызова, сек (по умолчанию 600; docker solc тянет образ).
- `SCSA_RPC_URL_<chain>` — RPC для `sca_target_value` (напр. `SCSA_RPC_URL_1`); иначе public-дефолт.

## Смоук-тест

```bash
printf '%s\n' \
'{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18"}}' \
'{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
| python3 scsa_mcp.py
```
