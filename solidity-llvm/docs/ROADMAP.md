# SCSA — дорожная карта усиления (многопроходная)

> Цель: закрыть слабые места, чтобы ловить БОЛЬШЕ реальных багов и меньше дублировать
> бесплатный Slither (дубликаты = $0 на баунти). Каждый проход = отдельная сессия с
> демонстрируемым артефактом. Инвариант каждого прохода: **corpus08 P/R держится, 191
> юнит-тест зелёный, добавлены и vuln-, и safe-кейсы (проверка на FP)**.
>
> Принцип «проходы закрывают слабые места друг друга»: фундаментальные проходы (2)
> разблокируют детекторные (3); финальный проход (5) — реальный бенчмарк из прошлых
> хаков + adversarial-FP-корпус — верифицирует ВСЕ предыдущие на боевых инцидентах.

## Слабые места (что чиним)
1. Внутрипроцедурно / модуль-на-контракт → cross-contract невидим (архитектура).
2. Read-only reentrancy (горячий класс 2023, Curve ~$70M) — не детектим.
3. Cross-function reentrancy — мелкий (трасса value рвётся на арифметике).
4. Oracle/price-манипуляция — самый оплачиваемый класс, вне охвата.
5. Прокси (EIP-1967/UUPS) — delegatecall в assembly не лоуэрится → невидимы.
6. OZ AccessControl `hasRole` (вложенные mapping) — современные протоколы невидимы.
7. Циклы — key-aware модель (сейчас откладываем как FP-склонное).
8. Нет бенчмарка на РЕАЛЬНЫХ хаках (recall на боевых инцидентах неизвестен).

---

## Проход 1 — Глубина reentrancy (низкий риск, ценно сразу)
**Закрывает:** #2, #3. **Риск:** низкий (single-file, переиспользует slot-машинерию).
- **Read-only reentrancy** (новый детектор, MEDIUM): слот S мутируется ПОСЛЕ value-вызова,
  а view-геттер G его читает/возвращает → во время callback G отдаёт устаревшее состояние
  (оракул/virtual_price обманывается). Класс мало кем ловится → мало дубликатов.
- **Cross-function reentrancy глубже:** трассировать value вызова через арифметику
  (add/sub/mul/div/urem), не только trunc/zext.
- Корпус: +read-only vuln + safe (view без пост-call-мутации), +cross-fn.

## Проход 2 — Cross-contract (2a детекция ✅, 2b фундамент ⏳)
**Закрывает:** #1. **Разблокирует:** проходы 3.
- **2a — детекция (СДЕЛАНО 2026-07-12, низкий риск):** эвристика cross-contract reentrancy
  (MEDIUM): ETH-value-вызов U, после которого достижим ДРУГОЙ внешний state-changing вызов E
  в другой контракт (напр. `ledger.debit()`) — во время callback из U контракт E не обновлён.
  Кейс `01_cross_contract_reentrancy`/Lendf.Me → находка. corpus08 23 файла P/R=1.0. Честная
  граница: это ФОРМА, не подтверждённый inter-procedural эффект.
- **2b — резолв + подтверждение (СДЕЛАНО 2026-07-12, контейнировано, низкий риск):**
  `implementerOf_` (interface→уникальный имплементер по linearizedBaseContracts) +
  `calleeWritesState` (резолв `obj.method()` → конкретная функция → проверка записи state-var в её
  теле). Если E пишет состояние → метка `!scsa.xcall.effect` → детектор повышает cross-contract
  MEDIUM→**HIGH (ПОДТВЕРЖДЁН)**. xc1/#01(Lendf.Me) → HIGH; нерезолвимый → MEDIUM. corpus08 24 файла
  P/R=1.0. Резолв-инфраструктура переиспользуется проходом 3 (oracle).
- **2b-full — inter-procedural фундамент (⏳ отдельная сессия, средне-высокий риск):** контракты в
  общий анализ-контекст + **неймспейсинг storage** (слоты B со смещением → без ложного алиасинга) →
  КОНКРЕТНЫЙ слот cross-contract эффекта + KLEE-подтверждение пути.

## Проход 3 — Oracle/price ✅ + accounting ⏳ (самый оплачиваемый класс)
**Закрывает:** #4. **Риск:** FP-склонно → тесная эвристика + форвард-тейнт.
- **Spot-price (СДЕЛАНО 2026-07-12):** `OraclePriceDetector`. Lowering распознаёт однозначные
  DEX-spot методы (`getReserves`/`slot0`/`getAmountsOut`/`getAmountIn`) → маркер
  `@scsa.oracle_read` (символьное i256). Детектор: цена форвард-тейнтом (через арифметику/
  приведения/phi/select) доходит до value внешнего вызова ИЛИ записи баланса → MEDIUM
  price-manipulation. НЕ включаем Chainlink `latestAnswer` (безопасен) и `balanceOf` (FP).
  Adversarial-safe: Chainlink → чисто, чтение-для-дисплея → чисто. corpus08 27 файлов P/R=1.0.
- **Balance-delta / fee-on-transfer (⏳):** кредит по `amount`, а не по измеренной дельте
  `balanceOf` до/после — отдельным заходом.

## Проход 4 — Прокси/upgrade + роли
**Закрывает:** #5, #6. **Риск:** средний.
- ✅ **4a — admin-гейт delegatecall (2026-07-12):** `guardedByMsgSenderBranch` в `DelegatecallDetector` —
  delegatecall, достижимый ТОЛЬКО через ветку сравнения msg.sender (control-dependent: ровно одна ветвь
  условного перехода доминирует блок), — admin-гейт (proxy `ifAdmin`, `require(msg.sender==admin)`), target
  доверенный. Убирает FP на `upgradeToAndCall`. Корпус: +dc1 (ifAdmin safe), +dc2 (unguarded → HIGH остаётся).
- ✅ **4b — пронзание прокси (2026-07-12, БОЛЬШОЙ выигрыш):** `EtherscanFetch` читает поля `Proxy`/
  `Implementation` из getsourcecode (Etherscan уже резолвит EIP-1967/Transparent impl-адрес) → авто-
  перенаправление на ИМПЛЕМЕНТАЦИЮ (1 хоп; fallback на обёртку, если impl не достать). **Не требует
  asm-delegatecall лоуэринга.** Проверено live: Aave `InitializableImmutableAdminUpgradeabilityProxy`→
  **PoolInstance** (0.8.27, 42 файла), Compound `TransparentUpgradeableProxy`→**CometWithExtendedAssetList**
  (0.8.15), EigenLayer→**StrategyManager** (0.8.30, 46 файлов). Все 3 импл-логики — **0 HIGH/MEDIUM FP**.
  Aave 2 FP (delegatecall на обёртке) → 0. **10-контрактный FP: 4 → 2** (осталось 1inch/2b + Sommelier/5).
- ⏳ **4c (осталось):** inline-asm `assembly { delegatecall(...) }` в лоуэринг (сейчас только member-call
  синтаксис — но пронзание прокси уже открыло impl-логику без него); OZ `hasRole` вложенный mapping
  `_roles[role].members[account]` как проверка прав (современные протоколы на ролях невидимы).

## Проход 6 — Распознавание OZ-гвардов через internal-вызовы ✅ (2026-07-12)
**Найдено FP-хардненингом (Проход 5) на 1inch.** OZ прячет гварды за слоями:
`onlyOwner`→`_checkOwner()`→`owner()`/`_msgSender()` → детекторы видели только тело функции → FP.
- **Сделано (контейнировано в детекторах, без риска инлайна):** `trace::collectMsgSenderFns`
  (функции, возвращающие msg.sender — `_msgSender`), `isAuthGuardFn` (условный revert по сравнению
  с msg.sender, в т.ч. через `_msgSender` — `_checkOwner`). SelfdestructDetector + AccessControlDetector:
  функция защищена, если ВЫЗЫВАЕТ auth-гвард-функцию (доминирующую опасную операцию). 1inch `destroy()
  onlyOwner` FP УШЁЛ; минимальный OZ-паттерн: guarded→чисто, unguarded→HIGH. corpus08 28 файлов P/R=1.0.
- **Router/simulate FP (СДЕЛАНО 2026-07-12):** (1) reentrancy теперь только на external/public
  функциях (метка `!scsa.internal` от lowering по `visibility`) — internal-хелперы под locked-
  родителем (UniversalRouter dispatch/v3SwapExactOutput) больше не FP; (2) DelegatecallDetector
  пропускает всегда-ревертящие функции (нет `ret`) — 1inch `simulate` больше не FP.
  UniversalRouter → чист; 1inch → без HIGH (осталась 1 MEDIUM-кандидат fillOrderTo).

## Проход 5 — Хардненинг: FP-sweep на реальном DeFi (ОТРЕЗВЛЯЮЩИЙ результат)
**Workflow-прогон на 15 аудированных mainnet-0.8 (Frax/Sommelier/Gearbox/Uniswap/1inch/Kyber/Morpho/
Aave): 32 HIGH+1 MEDIUM, ВСЕ 33 = FP, 0 TP.** Курируемый корпус был зелёный (только канонические формы) —
иллюзия; реальный код регрессирует, т.к. распознаватели гвардов принимали узкие IR-шаблоны. FP-классы найдены
и ЧАСТЬ исправлена:
- ✅ **equality-lock reentrancy-guard** (`require(locked==1)`/`_status!=_ENTERED` — Solmate/OZ ReentrancyGuard):
  `comparesLoadToConst` в isGuardedCall. Sommelier deposit/mint reentrancy FP ушли.
- ✅ **ERC4626-accounting слоты не привилегированы**: слот, писанный стандартной функцией (transfer/mint/
  deposit/withdraw/redeem/…) — учёт пользователей, исключаем из privilegedSlots. deposit/mint/withdraw/redeem
  AC FP ушли.
- ✅ **access-control только на external** (метка `!scsa.internal`): internal-хелперы под guarded-энтрипоинтом
  (Gearbox _setQuotaRevenue/_pause, Sommelier _disableInitializers/_enter) больше не FP.
  → **Sommelier: 9 FP → 2.** corpus08 30 файлов P/R=1.0.
- ✅ **Fix8 — lowering `.transfer`/`.send` (2026-07-12):** `token.transfer(to, amt)` на контракте/
  интерфейсе — это ERC20-метод (value=0), а НЕ нативный `address.transfer(value)`. Раньше args[0]
  (получатель) ошибочно брался как value → его storage-адрес попадал в value-трассу и давал мнимый слот
  → ложная cross-function reentrancy. Нативный ETH-перевод теперь только для address-получателя с 1 арг.
  **MOG clearStuckToken MEDIUM → 0 (mainnet).**
- ✅ **Fix6 — highCapable-пропагация через внутренние вызовы (2026-07-12):** внутренний вызов-триггер
  (F зовёт H, H делает внешний вызов) считается HIGH-триггером reentrancy ТОЛЬКО если вложенный вызов
  (транзитивно) шлёт ETH-value. value=0 вызов доверенного IRM/oracle (borrowRate/price) или ERC20 — не
  ETH-drain. Fixpoint по call-graph. **Morpho setFee/supply/withdraw/borrow/repay/liquidate 6×HIGH → 0.**
- ✅ **Fix4 — arbitraryTransfer только value≠0 (2026-07-12):** «неограниченный вывод средств» = отправка
  ETH-value. value доказуемо ноль (ERC20 token.transfer, staticcall, протокол-вызов) — ETH не покидает
  контракт → не вывод. **Morpho flashLoan HIGH → 0.** ERC20-вывод произвольному адресу честно НЕ детектим
  (получатель — аргумент, не виден в external_call-сигнатуре).
- ✅ **Fix2 — OZ initializer-guard (2026-07-12):** `initialize()` с модификатором `initializer` —
  разовая легитимная инициализация owner/config, не access-control-баг. Распознаём по имя-конвенции
  (initialize/reinitialize/__X_init) + структурному check-and-set одноразового скалярного слота
  (`_initialized` сравнивается с const И пишется). `gatesRevert` НЕ требуем (OZ компилит `||` в `or i256`
  через zext — узкий обход revert не проходит). Unprotected initialize (без модификатора) остаётся
  находкой. **Sommelier initialize HIGH → 0.** Корпус: +f1/f2 (Fix6 safe/vuln), +f3 (Fix8), +aw1 (Fix4),
  corpus08 34 файла P/R=1.0, 191/191.
- ⏳ **Осталось:** Sommelier `sendFees` (слот 17) — dual-use accounting-слот (пишется и owner-gated
  сеттером, и permissionless-функцией) → ложно привилегирован. Отдельный класс (не из Fix2/4/6/8).
  Также: слот-коллизия наследования (Sommelier: `_initialized` и symbol оба на слот 25 — баг storageLayout
  для inherited state-vars).

## Матрица возможностей (2026-07-12) — честная карта охвата классов (баг-баунти фокус)

`benchmarks/gen_capability.sh` → `benchmarks/capability/*.sol`: минимальная уязвимая проба на класс →
детектит/нет. Пивот на баг-баунти: метрика = ШИРОТА реального покрытия, не P/R на синтетике.

**✅ Ловит (ядро, низкий FP):** reentrancy, access-control, delegatecall (untrusted), **unprotected
initializer** (класс Nomad — Fix2 не сломал: init без init-lock остаётся access-control).

**✅ arbitrary external call — ПОКРЫТО (2026-07-12, `ArbitraryCallDetector`):** низкоуровневый `.call(data)`
(маркер `scsa.call.raw` от лоуэринга — отличаем от типизированного `token.transfer(...)` с фиксированным
селектором), где target восходит к АРГУМЕНТУ функции (attacker-controlled), value=0 (не ETH-перевод), а
точка входа НЕ под контролем прав (msg.sender-гейт/OZ-auth; blacklist НЕ считается защитой). **Реальный
recall: Socket WrappedTokenSwapper ($3.3M) пойман** (`performAction`/`performActionWithIn`). **0 FP на
UniversalRouter/1inch/Seaport/Morpho** (типизированные/декодированные вызовы не флагаются). Честные пропуски:
Seneca/Dexible (target из `abi.decode(bytes)` — не прямой аргумент), LiFi (EIP-2535 diamond — резолвится
один фасет). Расширение на декодированный target отложено (риск router-FP). corpus08 +ab1/ab2, 38 файлов P/R=1.0.

**❌ Дыры покрытия (за это платят баунти, мы не видим):**
| Класс | Почему пропуск | $ | Достижимость честно |
|---|---|---|---|
| **cross-contract / read-only reentrancy** (Penpie/NFT Trader/Sturdy/Conic) | модель «store-после-call в 1 функции» не видит | 🔴 | доминирует реальные 0.8-reentrancy; нужен guard-aware + межконтрактный |
| **first-deposit/share-inflation** (ERC4626) | нет детектора `supply==0?` | 🔴 | FP-склонно: безопасные OZ-vault (virtual shares) имеют ту же форму → нужен детект митигации |
| **signature-replay** (ecrecover без nonce) | ecrecover opaque | 🟠 | нужна ecrecover-модель + исключить permit с nonce-mapping |
| **price via balanceOf** (донат/flash-loan) | balanceOf исключён (FP) | 🟠 | FP-склонно |
| **fee-on-transfer/accounting-delta** | кредит по amt, не по дельте | 🟡 | FP-склонно (все ждут стандартный токен) |
| **unchecked ERC20 transfer** | только LOW | 🟡 | escalate до MEDIUM легко, но шумно |

**Честный вывод:** ни одна из 6 дыр не «дёшево и без FP» — все требуют либо детекта митигации, либо
IR/лоуэринг-расширения. Строить по данным (workflow recall на реальных хаках), а не вслепую — иначе
повторим ловушку синт-корпуса (canonical HIT ≠ real-world precision).

### Реальный recall (workflow `bug-recall-real-hacks`, 2026-07-12) — 15 захаканных 0.8-контрактов
**Поймано 1/11 анализируемых (~9%)** ДО arbitrary-call-детектора (только Socket Gateway через delegatecall).
Уроки: (1) боевые 0.8-хаки — это **arbitrary-call** (4/5 класса) и **cross-contract/read-only reentrancy**
(наша single-function модель их не видит) + oracle/inflation (вне досягаемости §7); (2) **инфра-потери 27%**
(4/15) — цель не дошла до анализа: proxy-резолв доверился полю Etherscan `Implementation` и пронзил в НЕ тот
контракт (Conic), выбор контракта по имени взял библиотеку а не логику (DFX `Curves`≠`Curve`), Etherscan V1
impl-endpoint мёртв (Earning.Farm); (3) **Penpie: 8 ЛОЖНЫХ access-control** — слот nonReentrant-гарда принят
за привилегированную запись. Приоритет (данные): **arbitrary-call ✅** → guard-aware precision-фикс (Penpie) →
cross-contract/read-only reentrancy → инфра (EIP-1967 slot-резолв, энум всех контрактов). НЕ строить:
price-oracle, first-deposit-inflation (честно вне досягаемости).

## Аудит готовности к продукту (2026-07-12, 16-агентный workflow) — вердикт NOT-READY, но резкий прогресс

**Эмпирика (10 боевых mainnet-0.8, LOW-шум исключён):** все 10 скомпилировались; 6-7 дали чистый ноль
(true-negative), 4 находки на 3 контрактах — **ВСЕ FP, 0 TP**. Агрегат: TP=0, FP=4, unc=0. Сравнение с
прошлым sweep: **33 FP → 4 FP** (шум −88%), но точность среди сработавших = 0/4, real-world TP по-прежнему 0.
- Sommelier `sendFees` — FP (dual-use timestamp-слот, известный).
- Aave v3 Pool ×2 — FP: анализировали ПРОКСИ-обёртку, а не имплементацию; `delegatecall` в `initialize`
  (за `require(_impl==0)` one-time guard) и `upgradeToAndCall` (за `ifAdmin`, immutable admin) → не эксплуат.
- 1inch `fillOrderTo` ×1 — FP: CEI соблюдён (эффект `_remaining[hash]` записан ДО внешних вызовов), а
  cross-contract-эвристика сработала на позднем refund.

**Определение «работающего продукта» (для баунти-хантера):** (1) находит РЕАЛЬНЫЙ баг на РЕАЛЬНОМ коде;
(2) HIGH/MEDIUM можно доверять; (3) пронзает прокси — анализ имплементации, не обёртки; (4) одна команда
адрес→зелёный форк-PoC против развёрнутой жертвы; (5) PoC доказывает ущерб assert'ом дельты. Ниже этого —
это «ещё один Slither» без денежного дифференциатора.

**Уточнённый порядок проходов (по деньги×достижимость, закрывают слабости друг друга):**
1. **Проход 4a — upgrade-guards (med, HIGH):** init-once (`require(_impl==0)`) + immutable-admin (`ifAdmin`,
   `msg.sender==immutable`) как гварды delegatecall → убирает 2/4 FP (Aave). Дёшево, до пронзания прокси.
2. **Проход 4b — пронзание прокси (high, HIGH):** EIP-1967/Transparent — определять proxy, тянуть impl-слот
   (Etherscan `getcontractcreation`/impl address), анализировать ИМПЛЕМЕНТАЦИЮ. 3/10 якорей — прокси, деньги
   там. + inline-asm `delegatecall` в лоуэринг (сейчас видим только member-call синтаксис).
3. **Проход 2b-refine — CEI-порядок cross-contract (med, MED):** засчитывать запись эффекта ДО всех внешних
   вызовов → снимает 1inch/router FP (любой роутер с корректным порядком + refund сейчас ложный MEDIUM).
4. **Проход 5 — реальный recall (high, HIGH, ЯДРО/линчпин диплома):** корпус ИЗВЕСТНЫХ хаков (DeFiHackLabs,
   0.8, verified, pre-patch блок) → измерить recall; закрыть dual-use FP (`sendFees`). **Цель: ОДИН
   воспроизведённый real-world TP через весь конвейер fetch→CFG→KLEE→форк-PoC→forge PASS.** Без этого и
   продукт, и новизна диплома недоказаны.
5. **Проход 6 — усилить AC-PoC оракул (low, HIGH):** сейчас тест «прошёл, если не ревертнул» — нет ассерции
   захвата owner/средств. Добавить `assertEq` состояния. Ров = доказательство, не «дошли сюда».
6. **Проход 7 — замкнуть воркфлоу (med, HIGH):** авто-проброс `--address`→`--victim`, фетч pre-patch блока
   (`getcontractcreation`), один зелёный форк-прогон. Путь адрес→PoC→сабмит сейчас рвётся между флагами.

**Честные риски (не переобещать):** (а) real-world TP может не появиться и после 4-5 — в аудированном коде
багов нет, реальные живут в свежих неаудированных протоколах (для диплома управляемо через DeFiHackLabs,
для продукта — экзистенциально); (б) прокси/asm/nested-mapping — работа во фронтенде, риск регресса синт-
корпуса; (в) модель гвардов точечная — новые паттерны рождают новые FP-классы, «низкий FP» заявлять только
на протестированном наборе; (г) KLEE деградирует там, где деньги (cross-contract эффект не моделируется,
per-user ключ схлопнут, глубина 1) — демо, не прод-гарантия; (д) НЕ обещать: logic errors, flash-loan,
полную oracle-манипуляцию, signature-replay, first-deposit/share-inflation, fee-on-transfer.

## Проход 5-old — Хардненинг: реальный бенчмарк + adversarial FP (верифицирует ВСЁ)
**FP-хардненинг на реальных mainnet-0.8 ЧАСТИЧНО (2026-07-12):** прогон Seaport/PepeToken/sDAI/
UniversalRouter/1inch/Permit2. Найдено+исправлено: (1) **remappings** — Etherscan multi-file settings
не передавались → solc падал на `@openzeppelin/`/`solmate/` импортах (UniversalRouter/Permit2 не
компилировались); (2) **value=0 reentrancy FP** — `dai.transferFrom`/`join`/protocol-вызовы (value=0)
перед записью баланса флагались HIGH → FP на КАЖДОМ депозите (sDAI ERC4626); теперь HIGH только для
ETH-value-отправляющих. Осталось: OZ-абстракция (→ Проход 6), router-гварды. Seaport/Permit2 → чисто.
**Закрывает:** #8 + проверяет проходы 1-4 на боевых инцидентах.
- **DeFiHackLabs-бенчмарк:** набор РЕАЛЬНЫХ прошлых хаков (0.8, verified) с известным
  классом → измерить recall на настоящих инцидентах (не на игрушках).
- **Adversarial-FP-корпус:** аудированные контракты (Seaport и т.п.) → должны быть чисты;
  ловит «крик волка» новых детекторов.
- KLEE cross-contract / multi-depth: символьное подтверждение новых классов.
- Итоговые P/R против Slither на реальном наборе.

---

## Статус
- [x] **Проход 6 — OZ-гварды** (2026-07-12): collectMsgSenderFns+isAuthGuardFn; selfdestruct/AC
      узнают `_checkOwner`-гвард → 1inch destroy FP убран. corpus08 28 файлов P/R=1.0.
- [~] **Проход 5 — хардненинг** (2026-07-12, частично): FP-прогон на mainnet → фикс remappings +
      value=0 reentrancy FP (sDAI); реальный recall-бенчмарк из хаков ⏳.
- [x] **Проход 3 — oracle/price** (2026-07-12): OraclePriceDetector (DEX-spot → форвард-тейнт →
      value/баланс), corpus08 27 файлов P/R=1.0. Accounting-delta ⏳.
- [x] **Проход 2 — cross-contract**: 2a детекция ✅ + 2b резолв→подтверждённый HIGH ✅
      (corpus08 24 файла); 2b-full (неймспейсинг storage + KLEE) ⏳ отдельной сессией.
- [x] **Проход 1 — глубина reentrancy** (2026-07-12): read-only reentrancy детектор
      (Curve-2023, MEDIUM, слот мутируется после value-вызова + view-геттер его читает) +
      cross-function глубже (трасса value через арифметику, `reachesStorageLoadDeep`).
      corpus08 21 файл P/R=1.0, 191/191. Оба класса — vuln+safe в корпусе.
- [ ] Проход 2 — cross-contract фундамент
- [ ] Проход 3 — oracle/price + accounting
- [ ] Проход 4 — прокси/upgrade + роли
- [ ] Проход 5 — хардненинг + реальный бенчмарк
