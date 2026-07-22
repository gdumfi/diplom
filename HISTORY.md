# HISTORY — журнал SCSA (краткий, онбординг для Клода)

Новые записи сверху. Это НЕ полный лог (детали — в git-истории `solidity-llvm/`), а выжимка,
чтобы новый Клод за 5 минут понял: что это, где что, и на какие грабли не наступать.
Структуру кода см. `solidity-llvm/docs/CODE-MAP.md`. Продукт-контекст — `CLAUDE.md`.

---

## 2026-07-22 — Критическая оценка «как стартап»: детектор не платит, фокус на инвариант-PoC

Провёл жёсткий аудит проекта по одному критерию — **принесёт ли деньги на баг-баунти** (прогон 201/201
тестов, чтение ядра детекторов, перепроверка КАЖДОЙ метрики против файлов бенчмарка двумя независимыми
аудит-агентами). Ключевые выводы записаны в **`solidity-llvm/docs/MONEY-STRATEGY.md`** (закоммичено):

- **Детектор ≠ деньги.** Силён там, где рынок вымыт (простая reentrancy/AC; протоколы гоняют Slither в
  CI до деплоя), слеп там, где деньги (inflation/oracle/replay/fee-on-transfer — собственный список
  `capability/*.sol // expect: miss`). На независимом SmartBugs SCSA = Slither = **28/31 (ничья)**.
- **Метрики завышены подачей:** headline «corpus08=14 файлов, P/R=1.0» устарел — на диске **38** файлов,
  новые детекторы в таблице не измерены; «28/29 vs 28/31» подано как победа, на равном знаменателе —
  ничья. Тест-счётчик тоже дрейфует: CLAUDE.md пишет 191, реально **201**. → задача P2 в MONEY-STRATEGY.
- **Ров (авто-PoC) — каркас.** Зелёный forge только на синтетике deposit+mapping; на боевой жертве —
  `TODO(hunter)` (форк-блок/конструктор/депозит руками). Подтверждает память `poc_autogen_boundary`.
- **Единственная денежная асимметрия:** `InvariantGen` (ERC4626/share-inflation, forge-fuzz) +
  bleeding-edge solc (0.8.31+, чужие тулзы не парсят) + `--address` throughput-воронка.
- **Решение:** режим «хантер с приватным инструментом», не SaaS. Все силы — в P0: довести инвариант-слой
  до боевого зелёного на реальном vault из scope = первый payout ($2–13K).

⚠️ В рабочем дереве `solidity-llvm/` на момент оценки лежали НЕзакоммиченные правки (детекторы, RESULTS.md
и др.) — не мои, не трогал, в коммит стратегии НЕ включал. Разобрать/закоммитить отдельно.

---

## TL;DR — что это и где мы

**SCSA** = анализатор безопасности смарт-контрактов. Транслируем Solidity → **LLVM IR** (не EVM-байткод,
как Slither/Mythril) и анализ строим поверх IR. Продукт = один бинарь `sca` (**AD-5: логика анализа только
в бинаре**; веб/TUI — тонкие «морды», дёргают его + парсят JSON).

- **Фронтенд 0.8:** solc `--standard-json` (разрешённый AST) → `codegen/AstJsonLowering` → EVM-размеченный IR
  (storage=`addrspace(1)`, `@scsa.external_call`, `@evm.*`). Рукописный legacy-фронтенд — под `--legacy`.
- **Детекторы** (CFG поверх IR): reentrancy (ядро), access-control (ставка #1 $953M), arbitrary-call, tx-origin,
  unchecked-call, missing-slippage, read-only-reentrancy, oracle-price/staleness, selfdestruct, delegatecall,
  weak-randomness.
- **KLEE-слой** (`--emit-klee` + `run_klee.sh`, docker `klee/klee`) — символьно подтверждает reentrancy/access-control.
- **Инвариант-харнессы** (`--emit-invariant`: solvency/inflation/inflation-4626/roundtrip/supply) — денежные
  классы через forge-fuzz; форк-режим против живой жертвы.
- **Ров (AD-7)** = авто-PoC (`--poc`, forge-verified). Метрики: **201 юнит-тест, corpus08 P/R=1.0**.
- **Продукт шире бинаря:** TUI (`sca tui`, `tui/`), витрина (React `web/` + Go-бэкенд `web/server/`),
  **ЖИВОЙ деплой** на homeserver → `https://scsa.95-165-81-234.sslip.io` (basic-auth). См. CODE-MAP §8b, `notes/`.
- **Диплом** (кафедра ИБ): новизна = метод трансляции 0.8→IR с семантикой EVM + переиспользование KLEE + авто-PoC.
  Защищать УЗКО (reentrancy до цифр), не переобещать.

---

## ⚠️ Тонкости / грабли — прочитать ДО работы

**1. Точность — это и есть ров** (Slither ловит паттерны бесплатно; наша ценность = мало FP + PoC).
Много труда ушло на антифолс-позитивы. **Уже закрытые FP-классы — НЕ переоткрывать, проверь ПЕРЕД «это баг!»:**
- `immutable owner` → чтение было =0, замок терялся. Фикс: `AstJsonLowering` моделирует immutable как storage-слот.
- **accounting-счётчики** (`totalStaked`/`totalFloat`/…) → `AccessControlDetector::writesSenderMapping`: функция,
  пишущая `mapping[msg.sender]` (per-user учёт: deposit/stake/`depositFloat`/…), НЕ флагается (имя-независимо);
  + `kAccountingFns` (имена deposit/mint/withdraw/stake/claim/…).
- **OZ AccessControl `onlyRole`** → `_checkRole`/`_checkOwner`/`_authorizeUpgrade` распознаны как guard ПО ИМЕНИ
  (проверка за вложенным role-mapping не видна структурно).
- Beefy `_checkManager` (`or`-guard `msg.sender==owner()‖==keeper`) → `IRTrace::condHasMsgSenderCmp` (icmp сквозь and/or).
- **slippage**: bare `swap` метится ТОЛЬКО для lib-helper (`DexSwap.swap`), контракт-`.swap()` (centralized swapper
  типа Beefy) — нет → modern Beefy чист, raw-router флаг. `amountOutMin=0` в permissionless harvest — реальный класс.
- read-only-reentrancy ужат: триггер только ETH-value>0 ИЛИ raw `.call` (иначе шум на yield-harvest).
- OZ-init/proxy-guard, accounting-слоты стандартных ERC20/4626-функций — де-привилегируются.
> Мораль: HIGH на access-control/slippage у свежего DeFi часто = accounting-FP или guard-за-функцией. Читай код.

**2. Операционная правда ханта** (см. память `hunting_ops_reality`):
- Тулза **0.8-only**. Рабочий путь = **`--address`** (Etherscan отдаёт self-contained flattened). Голые
  `git clone` без `forge install`/`npm install` → SKIP (импорты не резолвятся).
- **solc 0.8.31–0.8.36 не публикуются на Docker Hub** → `benchmarks/build_solc_image.sh <ver>` собирает свои
  образы из офиц. бинарей. Эдж: чужие тулзы с той же дырой bleeding-edge не видят, мы — да.
- **Hit-rate реальных зацепок ≈ 1/20.** Рынок чисто-контрактных багов выметен; современный 0.8-DeFi аккуратен
  (overflow мёртв, OZ безопасен, CEI знают). Payable решает **КОНТЕКСТ** (single-owner бот ≠ баунти;
  Beefy slippage = accepted-risk). Ключи: `ETHERSCAN_API_KEY` (обяз.), RPC/Alchemy (для форк-PoC).

**3. Docker / деплой:**
- solc/KLEE/foundry — через docker. solc идёт через **stdin** (`docker run -i … --standard-json <`, не `-v`),
  поэтому **docker-out-of-docker** (проброс `/var/run/docker.sock` в контейнер бэкенда) работает.
- Деплой на homeserver — за **существующим host-Caddy** (не поднимаем свой). Секреты в `stack.env` через
  `env_file` (иначе bcrypt-`$` в пароле ломает интерполяцию compose). Домена нет → **sslip.io**. Рантайм-образу
  нужны `python3` (EtherscanFetch зовёт) + docker-cli. Управление — `notes/DEPLOY-LIVE.md`.

**4. Правила проекта:** комментарии в C++ — **по-русски, с маленькой буквы**. Коммиты — БЕЗ Co-Authored-By
(диплом, авторство пользователя), email `gdumfi@yandex.ru`. **Держи CODE-MAP в синхроне** при структурных правках.
TUI собирается только Go ≥1.24 (Charm), локально `~/go-sdk`; веб — Node в `~/node` (виндовый npm ломается на WSL-путях).

---

## Хронология (сжато, новое сверху)

- **2026-07-21 (расширение охвата — reach)** — Две реальные цели подряд (Arcadia, Intuition) → «inapplicable»
  (аудированный код + скоуп вырезает access-control/классику; фетч не дотягивается за Etherscan). Диагноз
  подтверждён ROADMAP (0 real-world TP на аудированном, recall ~9%). Пользователь: **сперва СИЛЬНО расширить
  охват, потом LLM-петля** (см. память `decision_expand_reach_then_loop`). **Сделано (reach #1):** `EtherscanFetch`
  обобщён — флаг `--explorer-url https://<host>/api` (env `SCSA_EXPLORER_URL`) → `--address` работает на любом
  **Blockscout/Orbit-эксплорере** (L2/L3 без Etherscan-API: Intuition и т.п.). `resolveExplorer()` + мультифайл
  через `AdditionalSources`. Проброшено в MCP (`explorer_url`). Проверено live: WXDAI с Gnosis Blockscout достаётся.
  **+ Sourcify-фолбэк (reach #2):** если explorer молчит → авто-запрос к **Sourcify API v2** (`/v2/contract/{chain}/{addr}`,
  БЕЗ ключа, десятки сетей; v1 отключён «brownout»). Общий `parseExtractorOut` для всех источников. Проверено live:
  БЕЗ Etherscan-ключа достаёт **Permit2 (0.8.17, 30 файлов) с Sourcify** → компилит → анализирует. Теперь тул работает
  даже без Etherscan-ключа. 201/201 тестов зелёные. Осталось по reach: реестр L3 chainId→explorer, proxy на
  Blockscout, внешнее наследование.

- **2026-07-20 (research + MCP)** — Research-разбор state-of-the-art 2025 (Anthropic SCONE-bench: LLM+Foundry-форк
  генерят эксплойты 51% на 405 реальных; SymGPT LLM+symbolic; Halmos/Kontrol EVM-точная символика; Zhang-516:
  тулы покрывают ~20.5%; access-control 59% потерь H1'25). **Стратегия зафиксирована: LLVM+KLEE = метод,
  истина = Foundry-форк реального EVM** (снимает риск фиделити IR↔EVM). **Реализован MCP-сервер `mcp/scsa_mcp.py`**
  (stdlib Python, JSON-RPC/stdio, тонкая обёртка над `sca` — AD-5): 5 tools (`sca_audit`/`sca_context_pack`/
  `sca_poc`/`sca_invariant`/`sca_target_value`). Это нейро-символическая петля **без API-токенов** (подписочный
  Claude = LLM). Зарегистрирован в `.mcp.json`. План 4 расширений — `docs/EXPANSION-PLAN.md` (P1 MCP ✅, P2 TVL-приоритет,
  P3 CI/markdown, P4 hard-set датасеты = диплом, отложено). **Фокус = деньги баг-баунти, научка отложена.**

- **2026-07-20** — Bleeding-edge solc РАЗБЛОКИРОВАН (`build_solc_image.sh` собирает свои образы 0.8.31–0.8.36).
  **4-й FP-фикс** (структурный accounting: `writesSenderMapping`). Хант-раунд по свежим мультиюзер-DeFi
  (Base/BSC/OP/Polygon/ETH) — 0 payable (современный код чист; единственный HIGH `depositFloat` = FP → фикс).

- **2026-07-14 → 07-20** — Большая сессия: **3 FP-фикса** (immutable-owner · stake/totalStaked · OZ `onlyRole`)
  на живом ханте (~46 контрактов; гиганты Ethena/Lombard/SSV чисты, свежий кандидат отсеян контекстом).
  **Go-бэкенд** (`web/server/`, AD-5) + подключение фронта. **ЖИВОЙ деплой** на homeserver за host-Caddy.
  **TUI** (`sca tui`, Bubble Tea, тема сайта; вкладки Overview/IR-facts/PoC/Confirm). Страница `/cli`.
  Живые HuntQueue (`/api/batch`) + Invariants (`/api/invariant`).

- **2026-07-14** — Реальный хант Beefy/super-vaults: систематический класс **missing-slippage** (permissionless
  harvest, `amountOutMin=0`) + рефайн swapper-vs-router (снят FP на modern Beefy) + `_checkManager` guard-фикс
  (or-comb). Калибровка по топ-аудитору (pkqs90). Детектор **oracle-staleness**.

- **2026-07-13** — Форк-режим инварианта roundtrip (хант по адресу). read-only-reentrancy ловит call-триггеры.
  Старт веб-витрины (Vite+shadcn+Geist). Context-pack стал shape-aware (форма→готовая forge-команда). supply-инвариант.

- **2026-07-12** — **ПЕРЕЕЗД на Solidity 0.8** (solc-фронтенд в проде, P/R=1.0; рукописный — под `--legacy`).
  **Инвариант-слой B/B+** (solvency/inflation/inflation-4626/roundtrip/supply — денежные forge-свойства).
  **Нейро-символический слой** (`--llm-context` + `/hunt`: воронка sca→pack→Claude→доказательство). Пивот на
  баг-баунти (arbitrary-call детектор, матрица возможностей). Робастность фронтенда (SKIP-инструментовка).
  multi-file `--address` на боевых протоколах. Проходы усиления 1-5 (FP-фиксы на реальном 0.8-DeFi: прокси/upgrade/
  safeTransfer/accounting).

- **2026-07-11** — **KLEE-слой** (K1+K2: понижение EVM-IR, fork reentrancy глубины 1, оракул кражи, ktest→PoC без
  TODO — 3-й компонент диплома). Access-control в KLEE (обобщение на ставку #1). Slither-baseline (конкурентен+
  комплементарен). Наследование (OZ Ownable, клон 30/31). Робастность фронтенда 9→29/31 (recall 0.966). Форк-PoC
  против живой жертвы. Триаж «деньги×severity×KLEE». Бейдж `[SYM-CONFIRMED]`.

- **2026-07-10** — Глубокий холистический ревью (5 агентов): критичные баги найдены и починены.

- **Ранее (курсовая → диплом):** полный рукописный фронтенд (lexer/parser/sema/codegen), Этапы 0–7
  (storage=addrspace, external call, mapping, ReentrancyDetector, AccessControlDetector, `--audit`, бенчмарк).
