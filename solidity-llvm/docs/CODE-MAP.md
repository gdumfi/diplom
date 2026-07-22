# CODE-MAP — карта кода SCSA

> **Зачем этот файл.** Чтобы не перечитывать весь `src/` каждую сессию. Здесь: что где лежит,
> какой файл за что отвечает, ключевые символы и «когда сюда лезть». Читать ЭТОТ файл перед
> тем как искать код; открывать конкретный `.cpp` только когда карта указала — куда.
>
> **Актуальность.** Строки/факты сверены с кодом на **2026-07-12**. Числа строк — ориентир, дрейфуют.
> **Правило поддержки — см. низ файла:** добавил/переименовал/удалил файл, публичный класс, CLI-флаг
> или интринсик → обнови соответствующую строку здесь В ТОМ ЖЕ коммите. Иначе карта врёт и вредит.

---

## 0. Пайплайн за 30 секунд

```
                    ┌─ solc-путь (ПРОДУКТ по умолчанию) ─────────────────────────┐
.sol ──▶ Driver ──▶ SolcRunner (docker ethereum/solc:0.8.19 --standard-json)     │
                    │      └─ разрешённый AST (JSON) ──▶ AstJsonLowering ──┐      │
                    └─ --legacy: Lexer ▶ Parser ▶ Sema ▶ CodeGen ──────────┤      │
                                                                           ▼      │
                                              EVM-размеченный LLVM IR (in-memory Module)
                                                          │
                    ┌─────────────────────────────────────┼───────────────────────┐
                    ▼ analyzer/ (читает ТОЛЬКО IR)         ▼ klee/ (--emit-klee)    ▼ report/
          Reentrancy / AccessControl /            KleeLowering ──▶ KLEE-модуль   Finding ──▶
          PatternDetectors ──▶ Finding[]          (docker klee/klee) ──▶ witness  Console/Json/Sarif
                                                          │                        + PocGenerator (--poc)
                                                          └──── witness ───────────▶ подтверждённый PoC
```

**Инвариант архитектуры (проверять при правках):** downstream (`analyzer/`, `klee/`, `report/`)
читает **только LLVM IR**, `ast::` там встречается 0 раз. Поэтому фронтенд (solc- или legacy-путь)
можно менять, не трогая детекторы. Не нарушать — это несущая стена (см. CLAUDE.md §0, AD-3/AD-4).

**Семантика EVM в IR** (детали — [`IR-CONTRACT.md`](IR-CONTRACT.md)): storage = `ptr addrspace(1)`;
внешний вызов = `@scsa.external_call`; контекст = `@evm.msg_sender/msg_value/tx_origin/block_*`;
revert = `@evm.revert`; mapping-элемент = `@scsa.map_elem`; непрозрачная запись = `@scsa.opaque_store`;
чтение оракула = `@scsa.oracle_read`; selfdestruct = `@evm.selfdestruct`.

---

## 1. Фронтенд — .sol → LLVM IR

### 1a. solc-путь (продукт по умолчанию)
| Файл | Строк | Отвечает за | Ключевые символы |
|------|------:|-------------|------------------|
| `src/driver/SolcRunner.cpp` | 137 | Зовёт `docker ethereum/solc:<tag> --standard-json`, отдаёт разрешённый AST (JSON) | `SolcRunner`, `SolcResult` |
| `src/codegen/AstJsonLowering.cpp` | ~1300 | **Ядро продукта.** Лоуэринг solc-AST (JSON, парсится `llvm/Support/JSON.h`) в EVM-размеченный IR. Наследование/C3, struct-слои из `storageLayout`, инлайн модификаторов. **`immutable`-переменные НЕ в storageLayout → моделируются как синтет. storage-слоты** (обход `linearizedBaseContracts`, `mutability==immutable`); иначе их чтение = константа 0 и терялся guard `msg.sender!=owner` при `immutable owner` | `AstJsonLowering` |
| `include/solc/codegen/AstJsonLowering.h` | 141 | Публичный интерфейс лоуэринга | — |

### 1b. legacy-путь (рукописный, под `--legacy`; тест-скаффолд + сравнение)
| Файл | Строк | Отвечает за | Ключевые символы |
|------|------:|-------------|------------------|
| `src/lexer/Lexer.cpp` | 202 | Токенизация | `Lexer`, `Token` (`include/.../lexer/Token.h`) |
| `src/parser/Parser.cpp` | 963 | Recursive-descent → AST | `Parser` |
| `src/ast/AST.cpp` | 196 | Узлы AST | `AST.h`: `Expr`/`Stmt`/`ContractDecl`/`FunctionDecl`/`ModifierDecl`/`IndexExpr`/`LowLevelCallExpr`… |
| `src/sema/Sema.cpp` | 423 | Семантика для legacy-subset | `SemanticAnalyzer` |
| `src/sema/Inheritance.cpp` | 97 | Слияние base state-vars/функций/модификаторов | `mergeInheritance` |
| `src/sema/Symbol.cpp` | 33 | Таблица символов/скоупы | `Symbol`, `Scope`, `SymbolTable` |
| `src/codegen/CodeGen.cpp` | 732 | legacy AST → EVM-IR (та же разметка, что solc-путь) | `CodeGen`, `LocalSlot`, `StorageAlias` |
| `src/codegen/EvmIntrinsics.cpp` | 71 | Объявления `@evm.*`/`@scsa.*` интринсиков | `EvmIntrinsics` |

---

## 2. Анализатор — читает IR, выдаёт `Finding[]`  (`src/analyzer/`)

| Файл | Строк | Детектор / роль | Что ищет в IR |
|------|------:|-----------------|---------------|
| `src/analyzer/AnalysisPrep.cpp` | 26 | Подготовка IR к анализу | прогон `mem2reg` и т.п. → чистая SSA перед обходом |
| `src/analyzer/ReentrancyDetector.cpp` | 667 | **Reentrancy (ядро MVP)** | store в `addrspace(1)` после `@scsa.external_call` (CFG-reachability) + разбор nonReentrant-guard |
| `src/analyzer/AccessControlDetector.cpp` | ~310 | **Access control (ставка #1, $953M)** | owner-слот из `msg.sender==storage`; запись привилегированного слота без проверки. Скип guard-модификаторов (`hasGuardModifier`: `initializer`/`reinitializer`/`onlyInitializing`/`onlyProxy`/`notDelegated`). **Антифолс-позитив (важно, много правок):** (а) **OZ AccessControl** — `_checkRole`/`_checkOwner`/`_authorizeUpgrade` распознаются как auth-guard ПО ИМЕНИ (проверка за вложенным role-mapping + 2 internal-вызова, структурно не видна) → `callsAuthGuard` подхватывает вызывающих; (б) **accounting** — `kAccountingFns` (имена deposit/mint/withdraw/stake/unstake/claim/…) + структурный **`writesSenderMapping`**: функция, пишущая `mapping[msg.sender]` (per-user учёт: deposit/stake/`depositFloat`/…), НЕ флагается — её `total*` это счётчик, не привилегия (имя-независимо); (в) accounting-слоты стандартных ERC20/4626-функций де-привилегируются |
| `src/analyzer/PatternDetectors.cpp` | ~325 | Набор паттерн-детекторов | tx.origin (SWC-115), unchecked call (SWC-104 — **только low-level `.call`/`.delegatecall`/`.staticcall`/`.send`, метка `scsa.call.raw`**; высокоуровневые `transfer`/`safeTransfer` авто-реверят → не FP), selfdestruct, delegatecall, weak randomness, oracle price (spot-манипуляция), **oracle staleness** (Chainlink `latestRoundData`/`latestAnswer` — метка `scsa.oracle.chainlink` — в функции-потребителе без сверки `block.timestamp` → устаревшая цена; wrapper-фиды `latestRoundData`/… пропускаются), **missing-slippage** (DEX-своп: named-роутеры + generic `swap` через lib-helper `DexSwap.swap` — метка `scsa.swap`/интринсик `@scsa.swap`; флаг при (а) нет сравнения выхода с calldata-min-out ИЛИ (б) **permissionless harvest/reinvest со свопом без msg.sender-guard** — caller ставит minOut=0 и сэндвичит reward-своп; систематический класс yield-адаптеров, super-vaults 6/7. Граница: свап в non-harvest helper — межпроцедурный мисс) |
| `src/analyzer/IRTrace.cpp` | ~205 | Утилита трассировки IR | привязка находок к строкам/значениям, обход путей; **`isAuthGuardFn`** (auth-guard за internal-функцией типа OZ `_checkOwner`/Beefy `_checkManager`) — `condHasMsgSenderCmp` рекурсивно ловит icmp с msg.sender сквозь `and`/`or` (`msg.sender==owner()‖msg.sender==keeper`) → снят FP на `setKeeper` и т.п. |

**Классы детекторов** (заголовки): `ReentrancyDetector`; `AccessControlDetector`; в
`PatternDetectors.h` — `TxOriginDetector`, `UncheckedCallDetector`, `SelfdestructDetector`,
`DelegatecallDetector`, `WeakRandomnessDetector`, `OraclePriceDetector`, `OracleStalenessDetector`,
`ArbitraryCallDetector`, `SlippageDetector`.
> ⚠️ Расхождение с CLAUDE.md §3: таблица там перечисляет не все паттерн-детекторы —
> selfdestruct/delegatecall/weak-randomness/oracle добавлены проходами ROADMAP. Карта — источник правды по файлам.

---

## 3. KLEE-слой — символьное подтверждение  (`--emit-klee`)

| Файл | Строк | Отвечает за |
|------|------:|-------------|
| `src/klee/KleeLowering.cpp` | 404 | Понижение EVM-IR в KLEE-исполнимый модуль: storage→глобали, контекст/storage/block.*→символьные, revert→silent_exit, fork reentrancy глубины 1, оракулы, харнесс `main`. Режимы `Mode::Reentrancy` / `Mode::AccessControl` |
| `include/solc/klee/KleeLowering.h` | 40 | `KleeLowering`, режимы |

Прогон снаружи бинаря: `benchmarks/run_klee.sh` (docker `klee/klee` 3.2/LLVM16, `llvm-as` внутри
образа), батч — `benchmarks/run_klee_corpus.sh`. Witness из ktest → `PocGenerator`.

---

## 4. Репортинг и PoC  (`src/report/`)

| Файл | Строк | Отвечает за | Ключевые символы |
|------|------:|-------------|------------------|
| `include/solc/report/Finding.h` | 39 | Модель находки (severity, строка, функция, badge) | `Finding` |
| `src/report/ConsoleReporter.cpp` | 20 | Человекочитаемый вывод + `scoreFinding` (триаж-приоритет, дедуп, сортировка) | `ConsoleReporter` |
| `src/report/JsonReporter.cpp` | 53 | `--json` | `JsonReporter` |
| `src/report/SarifReporter.cpp` | 59 | `--sarif` (SARIF 2.1.0 для CI/GitHub) | `SarifReporter` |
| `src/report/ContextPackWriter.cpp` | 360 | **Сенсор для LLM-аналитика (`--llm-context`).** Собирает markdown-пак: факты из IR (слоты, ext-вызовы, kind, `$in`, owner-гвард, oracle, map[]) + зацепки детекторов + гипотезы-хуки под Solodit/SWC + **рекомендованные инварианты по форме** (ERC4626/payable-vault → готовая команда `verify_invariant.sh <src> <prop>`) + исходник. Скалярные записи (привилегия) отделены от map[msg.sender] | `ContextPackWriter`, `ContextMeta` |
| `src/report/PocGenerator.cpp` | 282 | **Ров (AD-7).** `--poc`: Foundry-PoC reentrancy / access-control; форк-режим (`vm.createSelectFork`); подтверждённый PoC из KLEE-witness | `PocGenerator`, `KleeWitness`, `ForkConfig` |
| `src/report/InvariantGen.cpp` | ~600 | **Фаза B/B+.** `--emit-invariant --invariant-property {solvency\|inflation\|inflation-erc20\|inflation-4626\|roundtrip\|supply}`: Foundry stateful-invariant харнесс. `solvency` (no-overdraw по reentrancy-находке); `inflation` (first-depositor, ETH); `inflation-erc20` (упрощённый ERC4626 `deposit(assets)`/`redeem(shares)`); **`inflation-4626` (СТАНДАРТНЫЙ ERC4626-интерфейс `deposit(assets,receiver)`/`redeem(shares,receiver,owner)` — применим к боевым vault-ам; валидирован end-to-end: ломается на наивном, ДЕРЖИТСЯ на OZ ERC4626/virtual-shares)**; `roundtrip` (`convertToAssets(convertToShares(x))<=x`, rounding; **`--victim` → ФОРК-режим против ЖИВОГО ERC4626 по адресу**); `supply` (unauthorized-mint). **6 денежных forge-свойств**, ловят классы, невидимые паттерн-детекторам | `InvariantGen`, `ForkConfig` |

---

## 5. Драйвер / CLI / фетч

| Файл | Строк | Отвечает за | Ключевые символы |
|------|------:|-------------|------------------|
| `src/main.cpp` | ~100 | CLI (cxxopts), разбор флагов → `Driver`. **Подкоманда `sca tui`** → `execvp` бинаря `scsa-tui` (Go/Bubble Tea, каталог `~/diplom/tui/`), себя передаёт как `SCA_BIN` | все `--флаги` (см. §7) + `tui` |
| `src/driver/Driver.cpp` | ~560 | Оркестрация: фронтенд → анализ → репорт/poc/klee; exit 2 при находках. **`--input` собирает транзитивное замыкание импортов** (`gatherLocalSources`/`scanImportPaths`): реальные репо (hardhat И foundry). Ключи sources = абсолютные пути. remap-правила: `remappings.txt` → `foundry.toml` (`remappings=[…]`) → авто-`lib/<name>/=lib/<name>/src` (foundry) → `node_modules/<pkg>` (hardhat); резолв по longest-prefix. Корень = ближайший предок с `remappings.txt`/`foundry.toml`/`node_modules`/`.git`. Одиночная фикстура без import-ов → 1 файл | `Driver`, `CompileOptions` |
| `src/driver/EtherscanFetch.cpp` | ~230 | `--address 0x… --chain N`: тянет верифицированный исходник из **трёх источников** (reach): (1) дефолт — единый **Etherscan V2 API** (по chainId, ключ `ETHERSCAN_API_KEY`); (2) `--explorer-url https://<host>/api` (env `SCSA_EXPLORER_URL`) → любой **Blockscout/Orbit-эксплорер** (L2/L3 без Etherscan-API, напр. Intuition) — тот же getsourcecode-формат + `AdditionalSources`; (3) **авто-фолбэк на Sourcify v2** (`/v2/contract/{chain}/{addr}`, БЕЗ ключа, десятки сетей) когда explorer молчит — работает даже без Etherscan-ключа. `resolveExplorer()` + `parseExtractorOut` (общий разбор) + `fetchFromSourcify`. Проверено live: Gnosis Blockscout (WXDAI) + Sourcify (Permit2 0.8.17, 30 файлов) | `Explorer`, `resolveExplorer`, `parseExtractorOut`, `fetchFromSourcify` |
| `src/diag/Diagnostic.cpp` | 9 | Диагностики/`SourceLoc` | `Diagnostic`, `DiagnosticEngine`, `SourceLoc` |

Точка входа: `main.cpp` → `Driver::run` (см. `Driver.cpp`). Бинарь `build/src/solidity-llvm`.

---

## 6. Тесты · бенчмарки · скрипты · docs

**Тесты** (`tests/`, GoogleTest, всего ~191 кейс). Кейсов на файл:
`test_parser` 50 · `test_sema` 28 · `test_analyzer` 23 · `test_codegen` 22 · `test_lexer` 18 ·
`test_access_control` 11 · `test_reporter` 11 · `test_klee` 9 · `test_pattern_detectors` 9 ·
`test_inheritance` 7 · `test_smoke` 3. Фикстуры — `tests/fixtures/*.sol`.

**Бенчмарки** (`benchmarks/`): `corpus/` (курируемый), `corpus08/` (боевой 0.8, P/R=1.0),
`smartbugs/` (независимый клон), `klee_access_control/`, `klee_negative/`,
`invariant_demo/` (Фаза B: `vault_vuln/safe.sol` — solvency; `share_vault_vuln/safe.sol` — ETH-inflation;
`erc4626_vuln/safe.sol` — ERC20-inflation; `erc4626_round_vuln/safe.sol` — roundtrip;
`token_mint_vuln/safe.sol` — supply (public mint vs onlyOwner). vuln ломают, safe держат).
`sample_vault.sol` — демо-мишень для реального `sca --json`/`--llm-context` (веб-консоль).
Прогон: `run_benchmark.sh`, `run_corpus08.sh`, `run_klee.sh`, `run_klee_corpus.sh`,
`compare_slither_docker.sh`, `verify_poc.sh`, `verify_invariant.sh` (генерит `--emit-invariant`
харнесс + реально гоняет `forge test`; сломанный инвариант = подтверждённый баг),
`hunt_batch.sh` (Фаза C — дешёвая воронка: `--dir`/`--addrs` → `sca --audit` пачкой →
`hunt_queue.md` с приоритетом выживших для `/hunt`; бакетит SKIP по коду причины `SCSA_SKIP:*`),
`hunt_feeder.sh` (ROBUSTNESS §0b — RPC block-walk → адреса верифиц. 0.8-контрактов для корпуса),
**`find_targets.py`** (автопоиск свежих оплачиваемых целей: активные контесты Sherlock/Cantina через
их JSON-API → `hunt/contests.md` + `targets.json` с репо/scope/приз/KYC; Code4rena закрывается → skip),
**`fetch_scope.py <#|title>`** (тянет in-scope .sol репо контеста мелкими запросами по commit →
`hunt/repos/<contest>/`; публичное `sherlock-audit/…` зеркало первым),
**`fetch_deps.py <#|title>`** (дотягивает git-submodule зависимости OZ/solmate/safe — их нет в
дереве как файлов; ТРАНЗИТИВНОЕ ЗАМЫКАНИЕ импортов через remappings+.gitmodules+gitlink-SHA →
только нужные файлы, не весь OZ; +remappings.txt/foundry.toml → sca/hunt_batch компилят) →
вход для `hunt_batch.sh --dir`,
**`build_solc_image.sh <ver…>`** (собирает локальный образ `ethereum/solc:<ver>` из офиц. статичного
бинаря `binaries.soliditylang.org` — Docker Hub перестал публиковать ~0.8.30, а свежие контракты идут
на 0.8.31–0.8.36; тулза подхватывает образ по тегу БЕЗ правок кода).
Результаты — `benchmarks/RESULTS.md`. **Коды SKIP** (`Driver::emitSkip`, см. ROBUSTNESS-PLAN §2):
A0 фетч · A1 нет образа solc (**лечится `build_solc_image.sh`**) · A2 не та версия · A3 ошибка компиляции ·
B1 сбой лоуэринга · C pre-0.8.

**Docs** (`docs/`): [`IR-CONTRACT.md`](IR-CONTRACT.md) — спецификация IR (интринсики, addrspaces);
[`ROADMAP.md`](ROADMAP.md) — 5 проходов усиления; [`HUNT-PLAYBOOK.md`](HUNT-PLAYBOOK.md) —
методичка нейро-символического хантинга (для `--llm-context` + `/hunt`);
[`HUNT-LOOP-PLAN.md`](HUNT-LOOP-PLAN.md) — **автономный хант-цикл** для `/loop`: оркестрация
`hunt_feeder→hunt_batch→/hunt→forge`, модель состояния в `~/diplom/hunt/` (seed/queue/processed/
findings/log), границы честности, готовый промпт §7;
[`ROBUSTNESS-PLAN.md`](ROBUSTNESS-PLAN.md) — data-driven план снятия SKIP фронтенда на реальных 0.8;
**этот файл** — карта кода.

**Сборка:** CMake+Ninja, LLVM 18.1. `CMakeLists.txt` (корень) → `src/CMakeLists.txt`
(либа `solc_core` + бинарь `solidity-llvm`) → `tests/CMakeLists.txt`.

---

## 7. CLI-флаги (`main.cpp`)

`-i/--input` · `--address` · `--chain` · `--explorer-url`(Blockscout/Orbit reach) · `-o/--output` · `--emit-ir` · `--legacy` · `--solc-version`
· `--audit` · `--json` · `--sarif` · `--llm-context` · `--min-severity` · `--confirmed` · `--poc`
· `--emit-invariant` · `--invariant-property`(solvency|inflation|inflation-erc20|inflation-4626|roundtrip|supply)
· `--emit-klee` · `--klee-entry` · `--klee-mode`(reentrancy|access-control) · `--klee-slot`
· `--klee-owner-slot` · `--klee-confirmed` · `--klee-balance` · `--klee-msg-value` · `--klee-args`
· `--victim` · `--fork-block` · `--fork-rpc-env` · `-h/--help`.

---

## 8. «Куда лезть» — быстрый индекс задач

| Задача | Файл(ы) |
|--------|---------|
| Добавить/поправить трансляцию Solidity-конструкции (продукт) | `codegen/AstJsonLowering.cpp` |
| То же для legacy-пути | `parser/Parser.cpp` → `sema/Sema.cpp` → `codegen/CodeGen.cpp` |
| Новый/изменённый интринсик или addrspace | `codegen/EvmIntrinsics.cpp` + `docs/IR-CONTRACT.md` (+ детектор-потребитель) |
| Правка reentrancy-логики / guard | `analyzer/ReentrancyDetector.cpp` |
| Правка access-control | `analyzer/AccessControlDetector.cpp` |
| Новый паттерн-детектор | `analyzer/PatternDetectors.{h,cpp}` (+ регистрация в `Driver.cpp`) |
| KLEE-лоуэринг / оракул / режим | `klee/KleeLowering.cpp` |
| Шаблон PoC / форк-режим / witness | `report/PocGenerator.cpp` |
| Инвариант-харнесс (solvency/no-overdraw) | `report/InvariantGen.cpp` (+ прогон `benchmarks/verify_invariant.sh`) |
| Батч-воронка (много контрактов → триаж-очередь) | `benchmarks/hunt_batch.sh` (Фаза C) |
| Новый CLI-флаг | `main.cpp` (объявление) + `Driver.cpp` (обработка) |
| Формат вывода (console/json/sarif) | `report/*Reporter.cpp` |
| Контекст-пак для LLM-анализа (`--llm-context`) | `report/ContextPackWriter.cpp` (+ плейбук `docs/HUNT-PLAYBOOK.md`, команда `.claude/commands/hunt.md`) |
| Триаж/скоринг находок | `report/ConsoleReporter.cpp` (`scoreFinding`) |
| Фетч по адресу | `driver/EtherscanFetch.cpp` |

---

## 8b. Продукт целиком — соседние каталоги (`~/diplom/`, вне `solidity-llvm/`)

Бинарь `sca` = ядро, но продукт шире. На уровне `~/diplom/`:

| Каталог | Что | Ключевое |
|---|---|---|
| `tui/` | **TUI** (Go + Bubble Tea) — команда `sca tui`. Тонкая морда (AD-5): дёргает `sca --json`/`--llm-context`/`--poc`, рисует. | `main.go`/`update.go`/`view.go`/`theme.go`; тема `#e6b34a`; ввод→скан→список→деталь (вкладки Overview/IR-facts/PoC/Confirm, `y`=copy); `start-tui.sh`, `README.md`. Bubble Tea требует Go ≥1.24 (локально в `~/go-sdk`) |
| `web/` | **Витрина** (React 19 + Vite + Tailwind + shadcn). | `src/pages/` (Landing/NewScan/Findings/FindingDetail/HuntQueue/Invariants/InstallCli); `lib/scan.tsx`+`lib/api.ts` (вызовы бэка); демо-данные `data.ts`/`queue-data.ts` (фолбэк, если бэк не отвечал) |
| `web/server/` | **Go-бэкенд** (тонкий, AD-5, stdlib): дёргает `sca`, парсит JSON, раздаёт SPA. | `main.go` (`/api/scan`,`/api/health`,статика), `batch.go` (`/api/batch` воронка · `/api/invariant` харнесс). `README.md` |
| `deploy/` | **Пакет деплоя** VPS/homeserver. | `Dockerfile` (sca+фронт+Go+docker-cli), `docker-compose.yml` (+`.homeserver.yml` backend-only), `Caddyfile`, `stack.env.example`. solc через проброшенный `docker.sock` (stdin, не `-v` → docker-out-of-docker) |
| `mcp/` | **MCP-сервер над `sca`** — делает бинарь вызываемым инструментом для подписочного Claude Code = нейро-символическая петля без API-токенов (AD-5: тонкая обёртка, stdlib-only Python, JSON-RPC 2.0/stdio). | `scsa_mcp.py` (5 tools: `sca_audit`/`sca_context_pack`/`sca_poc`/`sca_invariant`/`sca_target_value`), `README.md`. Зарегистрирован в `~/diplom/.mcp.json` (сервер `scsa`). env `SCA_BIN`/`SCA_TIMEOUT`/`SCSA_RPC_URL_<chain>`. План расширений — `docs/EXPANSION-PLAN.md` |

**ЖИВОЙ деплой:** homeserver `192.168.1.8` (публ. `95.165.81.234`), за существующим host-Caddy →
`https://scsa.95-165-81-234.sslip.io` (basic-auth). Управление/апдейт — `notes/DEPLOY-LIVE.md`.

---

## 9. Как поддерживать эту карту (правило проекта)

Карта полезна ровно пока не врёт. Поэтому **в том же коммите**, что меняет структуру:

1. **Добавил/удалил/переименовал `.cpp`/`.h`** → правь §1–5 (строка файла) и, если поменялась
   точка входа, §8.
2. **Новый публичный класс/детектор** → §2 (или профильный раздел) + §8.
3. **Новый CLI-флаг** → §7 + §8.
4. **Новый `@scsa.*`/`@evm.*` интринсик или addrspace** → §0 (список) + `IR-CONTRACT.md`.
5. **Сдвиг пайплайна** (новый этап, смена дефолтного пути) → §0 схема.

Числа строк — ориентир, не сверять педантично; но если файл вырос/усох в разы — обнови.
Если правка большая и структурная — проще перегенерировать карту, чем латать (быстрый осмотр:
`find src include -name '*.cpp' -o -name '*.h' | xargs wc -l` + `grep -rE '^\s*(class|struct)' include/`).

> Это не «документация ради документации»: карта существует, чтобы Клод (и человек) находил код
> без повторного сканирования всего дерева. Устаревшая карта хуже отсутствующей — держи в синхроне.
