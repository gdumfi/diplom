# SCSA web backend (Go)

Тонкий слой по **AD-5**: не содержит логики анализа. Только дёргает бинарь
`solidity-llvm` (продукт), парсит его `--json`, маппит в модель фронта и раздаёт
собранный React (`web/dist`). Логику анализа сюда **не переносить** — она в C++.

## Сборка

```bash
export PATH=$HOME/go-sdk/go/bin:$PATH   # или системный go ≥ 1.23
cd web/server
go build -o scsa-server .
```
Зависимостей нет (только стандартная библиотека) — собирается офлайн.

## Запуск

```bash
# бинарь анализатора и папка со статикой — через env (у путей есть дефолты)
export SCA_BIN=../../solidity-llvm/build/src/solidity-llvm
export WEB_DIST=../dist
export ETHERSCAN_API_KEY=...            # нужен ТОЛЬКО для скана по адресу
./scsa-server                            # слушает :8080 (PORT переопределяет)
```

## Конфигурация (env)

| Переменная | Дефолт | Назначение |
|---|---|---|
| `PORT` | `8080` | порт HTTP |
| `SCA_BIN` | `../../solidity-llvm/build/src/solidity-llvm` | путь к бинарю анализатора |
| `WEB_DIST` | `../dist` | папка собранного фронта |
| `SCAN_TIMEOUT` | `240s` | потолок на один скан |
| `ETHERSCAN_API_KEY` | — | пробрасывается в бинарь для `--address` |

## API

### `GET /api/health`
```json
{ "ok": true, "binary": "…/solidity-llvm" }
```

### `POST /api/scan`
Тело — по адресу **или** по исходнику:
```json
{ "mode": "address", "address": "0x…", "chain": 1 }
{ "mode": "source",  "name": "Vault.sol", "source": "pragma solidity ^0.8; …" }
```
Ответ (модель фронта, см. `web/src/data.ts`):
```json
{
  "target":   { "name": "0x9d39…3497", "source": "etherscan · chain 1", "chain": 1 },
  "findings": [
    { "id":"f1","sev":"HIGH","rule":"reentrancy","fn":"withdraw()",
      "line":10,"status":"candidate","score":340,"message":"…" }
  ]
}
```
Маппинг вывода бинаря → фронта: `MEDIUM→MED`; `symConfirmed→status:"klee"`,
иначе `LOW→"info"`, прочее `"candidate"`; `function→fn+"()"`; `priorityScore→score`.
Exit-код бинаря `2` («находки есть») ошибкой **не** считается.

## Dev-режим (фронт отдельно)

```bash
./scsa-server &                 # бэк на :8080
cd .. && npm run dev            # vite на :5173, /api проксируется на :8080 (vite.config.ts)
```

## Prod-режим (один процесс)

```bash
cd .. && npm run build          # → web/dist
cd server && ./scsa-server      # бэк раздаёт и API, и статику с одного порта
```

## Безопасность

Без шелла (аргументы бинаря передаются напрямую), адрес валидируется regex,
исходник ограничен 2 МБ, на каждый скан — таймаут. Компиляция недоверенного
исходника уходит в `solc` внутри docker (изоляция на стороне бинаря).
