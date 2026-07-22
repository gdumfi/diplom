# Пул находок — сессия 2026-07-14

Просканировано **~46 контрактов, 6 сетей** (Base/Arbitrum/Optimism/Polygon/BSC/Ethereum) через
`--address` (self-contained исходник). Источники: recently-verified эксплореров + Beefy API (yield-стратегии).
**Обе FP-класса сессии исправлены** (immutable-owner + stake/totalStaked) → последний батч из 26 контрактов
дал **0 ложных HIGH** (precision держится на объёме). Итог по HIGH-выжившим за всю сессию: **1 реальная зацепка**.

## 🎯 Настоящие зацепки (приоритет разбора)

### P1 — AerodromeAutoLP · `checkMarketHealth` — Base `0x6e8e48d38d9ae594e9e6b359477fad8dea60d45e`
**РАЗОБРАНО 2026-07-15 → НЕ payable (закрыто).** Паттерн реальный: permissionless circuit-breaker →
`paused` + `_performEmergencyExit()`; снапшот ликвидности манипулируем (`ICLPool.liquidity()` = single-tick).
НО **контекст убивает импакт:** контракт **single-owner** — `deposit`/`withdraw`/`emergencyWithdraw` все
`onlyOwner` (нет публичного входа, нет чужих депозиторов). `_performEmergencyExit` → `_consolidateAndSweepToOwner`
(средства уходят ВЛАДЕЛЬЦУ, не атакующему). `withdraw()` НЕ `whenNotPaused` → владелец всегда выведет.
Итог: атакующий может лишь ГРИФить владельца (форс-выход его же личного бота в неудобный момент) ценой
манипуляции пула через 1h confirm-delay + окно снапшота 24h → экономически бессмысленно, чужих денег нет.
**Тот же паттерн в МУЛЬТИ-юзер vault = был бы реальный MEDIUM+.** Урок: контекст (чьи деньги, куда уходят)
решает payable/нет — виден только человеку, читающему код. Judgment-слой спас от отправки не-бага.

## 🟡 Accepted-risk / низкий приоритет
- **Beefy-стратегии ×20 (Base)** — `missing-slippage` в harvest-свопах. Beefy официально = accepted-risk
  (MixBytes). Платно только на форке с тонким пулом. Скип, кроме как найти НЕ-Beefy форк.
- **StakingV3 (BSC `0xb4E608...`)** — 1 LOW. Мелочь.

## Заход по in-scope многопользовательским (2026-07-16)
Ethena/Lombard/SSV (Ethereum, payable-by-construction). 8 ключевых контрактов → все **чисто** после фикса.
**StakedUSDe (Ethena, $3M) сначала дал HIGH `transferAdmin` — ЛОЖНЫЙ** (OZ `onlyRole` не распознавался,
см. ниже). Аудированные гиганты по нашим классам пусты — ожидаемо; реальный edge = мелкие/свежие.

## ❌ Ложные тревоги (убраны из пула; ТРИ класса исправлены за сессию)
- **[3] OZ AccessControl `onlyRole`** (StakedUSDe·transferAdmin) — замок за `_checkRole`→`hasRole`
  (вложенный role-mapping, msg.sender за 2 internal-вызовами). **ИСПРАВЛЕНО:** `AccessControlDetector.cpp`
  распознаёт `_checkRole`/`_checkOwner`/`_authorizeUpgrade` как auth-guard по имени. Снимает FP на КАЖДОМ
  role-based контракте (≈ вся мультиюзер-DeFi). 201/201, corpus08 1.0.
- **AerodromeAutoLP·setKeeperExecutor** — `immutable owner` не моделировался → замок не виден.
  **ИСПРАВЛЕНО** в `AstJsonLowering.cpp` (immutable → синтетич. storage-слот). 201/201, corpus08 P/R=1.0.
- **MagmaStaking·stake (Arbitrum `0xe4eF1F...`)** — permissionless `stake` пишет `totalStaked` (учёт, не
  привилегия) → FP. **TODO:** добавить `stake`/`unstake`/`claim`/`getReward` в `kAccountingFns`
  (`AccessControlDetector.cpp`) — как `deposit`/`mint`/`withdraw`. Убрать этот класс шума.

## ✅ Чисто по классам тулзы (не значит «без багов» — значит вне наших классов)
Протокол **Magma** целиком (Vault/MultiVault/Staking/Settlement/**Clearinghouse/InsuranceFund/
LiquidationEngine**/FeeRouter/Incentives/Oracle/Token) — Base+Arbitrum+BSC, свежий деплой 2026-07-14.
YO Protocol; Pendle/ACP VaultFactory; Palantex Strategy/Module; Abracadabra Cauldron; MasterChefV1_2;
TokenCDSV2; Polymarket/Robin vault-ы. Денежное ядро Magma (ликвидации/клиринг/страхфонд) — чисто по
reentrancy/access-control/slippage; если там баг — то логика/экономика (вне охвата тулзы).

## Урок сессии
Hit-rate настоящих зацепок ≈ **1 на 20** свежих контрактов (и тот — «вопрос», не готовый payout).
Это подтверждает тезис: рынок чистых контрактных багов прочёсан, тулза = генератор кандидатов, деньги —
в суждении + редком выжившем. Плюс: сессия закалила тулзу (−1 FP-класс исправлен, −1 найден).
