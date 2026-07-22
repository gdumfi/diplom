# AC-HASROLE-PLAN — распознавание OZ `hasRole()`-гвардов (закрытие ROADMAP #6)

> Цель: убрать доминирующий FP-класс `AccessControlDetector` на современных протоколах —
> guard через OZ AccessControl `hasRole()`/`onlyRole` (вложенный role-mapping), в т.ч. за
> кастомной internal-функцией (`_requireManagerRole`) и operator/param-mapping (`_isOperator[a][msg.sender]`).
> Мотивировано ПРЯМЫМИ уликами: Tare-триаж дал 4 AC-кандидата, ВСЕ FP этого класса.
>
> Инвариант правки (жёсткий): **corpus08 P/R=1.0 держится, 201 юнит-тест зелёный, добавлены и
> vuln- и safe-кейсы**. Не над-подавлять: реально незащищённая привилегированная запись остаётся HIGH.

---

## 1. Корневая причина (по коду, сверено)

Текущее распознавание гвардов (`AccessControlDetector.cpp` + `IRTrace.cpp`):
- `hasOwnerCheck` — `require(msg.sender == storage[slot])` (icmp msg.sender vs storage).
- `hasRoleCheck`→`reachesRoleLoad` — branch-to-revert, чьё условие ИНЛАЙН достигает загрузки
  `mapping[…msg.sender…]` (одиночный `@scsa.map_elem`, ключ = msg.sender). Ловит `require(admins[msg.sender])`.
- `authGuards` = { fn : `isAuthGuardFn(fn)` ∨ имя ∈ `{_checkRole*,_checkOwner,_authorizeUpgrade}` };
  `callsAuthGuard(fn)` = fn зовёт функцию из `authGuards`. Ловит `onlyOwner→_checkOwner()`.
- `isAuthGuardFn`→`condHasMsgSenderCmp` — branch-to-revert, чьё условие достигает `icmp` с msg.sender
  (в т.ч. сквозь `and/or/xor`). НЕ заходит внутрь вызовов.

**Два пробела, дающие FP на OZ AccessControl:**
- **(A) guard за вызовом role-query.** `_requireManagerRole()` ревертит по результату `hasRole(role, msg.sender)`.
  `hasRole` — отдельная функция; ни `condHasMsgSenderCmp`, ни `reachesRoleLoad` не заходят в её тело.
  Имя `_requireManagerRole` не в списке → не `authGuard` → вызывающий (`updateNav`) флагается.
- **(B) вложенный role-mapping.** `_roles[role].members[msg.sender]` — ДВА `@scsa.map_elem`. `reachesRoleLoad`
  проверяет ключи `map_elem` на msg.sender и это работает для загрузки (ключ msg.sender есть), НО загрузка
  спрятана в теле `hasRole` (см. A), поэтому не достигается.

Итог: нужно научиться распознавать **role-query-функции** (`hasRole`-подобные) структурно и считать
revert, гейтящийся их результатом с `msg.sender` в роли account-аргумента, — проверкой прав.

---

## 2. Дизайн фикса (структурный, name-independent)

### 2.1 Новый примитив: `isRoleQueryFn` (в `IRTrace`)
```cpp
// F — «запрос членства/роли»: ВОЗВРАЩАЕТ значение, восходящее к загрузке mapping[...][P],
// где P — АДРЕСНЫЙ ПАРАМЕТР F (ключ mapping). Покрывает:
//   OZ hasRole(bytes32 role, address account) = _roles[role].members[account] (вложенный mapping)
//   любой isOperator/isWhitelisted(address a) -> mapping[a]
// name-independent. accountArgIdx ← индекс параметра-ключа (для сверки, что в вызове туда идёт msg.sender).
bool isRoleQueryFn(const llvm::Function& F, int& accountArgIdx);
```
Реализация: для каждого `ReturnInst` трассируем возвращаемое значение НАЗАД (через `icmp/trunc/zext/
and/or/xor/not`) до `LoadInst` из `@scsa.map_elem`, чей КЛЮЧ-аргумент — `llvm::Argument` функции F
(не msg.sender, не константа, не storage). Возвращаем idx этого аргумента. Вложенность (роль→members→account)
покрыта тем, что нас интересует ВНУТРЕННИЙ ключ = account-параметр.

### 2.2 Расширить `condHasMsgSenderCmp` (в `IRTrace`) — заход через role-query вызов
Добавить ветку: если узел (или операнд `icmp`/`and`/`or`/`xor`) — это `CallInst` к F, и
`isRoleQueryFn(F, idx)` истинно, и `reachesMsgSenderX(call->arg(idx))` (в account-аргумент идёт msg.sender)
→ вернуть true. Тогда:
- `isAuthGuardFn(_requireManagerRole)` = true (revert по `!hasRole(role,msg.sender) && …`).
- `_requireManagerRole` попадает в `authGuards` → `callsAuthGuard(updateNav)` = true → **updateNav не флагается**.
- Прямой инлайн `require(hasRole(role, msg.sender))` в самой точке входа тоже ловится (isAuthGuardFn).

### 2.3 Расширить `reachesRoleLoad` (в `AccessControlDetector`) — тот же заход через role-query
Симметрично: при встрече `CallInst` к role-query-F с msg.sender в account-арге — считать «достигнута
role-load». Покрывает `hasRoleCheck` на точке входа с инлайн-`require(hasRole(...))` без промежуточной функции.

### 2.4 Operator/param-guard (`onlyAccountOrOperator`) — пункт (B) для requestDeposit/cancel
Гвард `require(msg.sender == account || _isOperator[account][msg.sender])`. Из-за short-circuit `||`
компилится в НЕСКОЛЬКО блоков; проверить, что `hasRoleCheck` ловит блок с `_isOperator[account][msg.sender]`
(ключ msg.sender на 2-м уровне вложенности → `reachesRoleLoad` по загрузке уже должен матчить). Если из-за
разбиения на блоки не ловит — расширить `hasRoleCheck`: собрать ЛЮБОЙ revert-block и проверять условия ВСЕХ
доминирующих ветвлений, не только терминатор одного bb. (Диагностика на этапе реализации — см. §4.)

### 2.5 (Опционально, отдельный заход) cross-contract role-registration
`createOffer`: `require(ILoansAuth(x).isRegisteredForRole(msg.sender, …))` — ВНЕШНИЙ вызов, тело не видно.
Плюс слот 3 = `offerCount` (счётчик-инкремент). НЕ включаем в этот фикс (риск подавить реальные баги
внешним вызовом). Отдельные классы: (i) «revert по внешнему view-вызову с msg.sender-аргументом» —
слабый auth-сигнал, только если вызов `staticcall`; (ii) монотонный счётчик `++x` — не привилегия.
Помечено как future в §6.

---

## 3. Точки правки (файлы/символы)

| Файл | Правка |
|---|---|
| `include/solc/analyzer/IRTrace.h` | объявить `bool isRoleQueryFn(const llvm::Function&, int& accountArgIdx);` |
| `src/analyzer/IRTrace.cpp` | реализовать `isRoleQueryFn`; расширить `condHasMsgSenderCmp` (заход через role-query call) |
| `src/analyzer/AccessControlDetector.cpp` | расширить `reachesRoleLoad` (заход через role-query call); при построении `authGuards` уже подхватится через `isAuthGuardFn`. Если нужно — добавить `hasRoleCheck(fn)`-функции в `authGuards` |
| `benchmarks/corpus08/` | +vuln +safe фикстуры (§5) |
| `tests/test_access_control.cpp` | +юнит-кейсы (§5) |
| `docs/CODE-MAP.md`, `docs/ROADMAP.md` | отметить #6 закрытым; обновить строки детектора/IRTrace |

---

## 4. Порядок реализации (пошагово, с проверкой на каждом)

1. **Написать `isRoleQueryFn`** + юнит-микротест на минимальном `hasRole`-подобном IR.
2. **Расширить `condHasMsgSenderCmp`** → пересобрать → проверить, что `isAuthGuardFn` ловит
   синтетический `_requireManagerRole`-паттерн (safe-фикстура §5 становится CLEAN).
3. **Расширить `reachesRoleLoad`** (инлайн-случай) → проверить safe-инлайн `require(hasRole(...))`.
4. **Прогнать corpus08** (`run_corpus08.sh`) — P/R ДОЛЖЕН остаться 1.0 (0 регрессий на старых кейсах).
5. **Прогнать 201 юнит-тест** — зелёные + новые кейсы.
6. **Диагностика requestDeposit/cancel** (пункт 2.4): если ещё FP — доработать `hasRoleCheck` по доминаторам.
7. **Re-триаж Tare** (`triage_tare.sh`): updateNav/requestDeposit/cancel → CLEAN; createOffer может остаться
   (см. 2.5) — задокументировать честно.
8. **Adversarial**: убедиться, что реально НЕзащищённая привилегированная запись (setOwner без проверки)
   ВСЁ ЕЩЁ HIGH (vuln-фикстура §5). Не над-подавили.

---

## 5. Корпус (обязателен — защита от регресса)

**safe (должны стать/остаться CLEAN):**
- `ac_hasrole_safe.sol` — OZ AccessControl: `updateX() { _requireManagerRole(); priv = ...; }`,
  где `_requireManagerRole` ревертит по `hasRole(ROLE, msg.sender)` (вложенный `_roles[role].members[]`).
- `ac_hasrole_inline_safe.sol` — `require(hasRole(ROLE, msg.sender)); priv = ...;` прямо в точке входа.
- `ac_operator_safe.sol` — `onlyAccountOrOperator`: `require(msg.sender==a || _isOperator[a][msg.sender])`.

**vuln (должны ОСТАТЬСЯ HIGH — не над-подавили):**
- `ac_hasrole_vuln.sol` — контракт С ролями, но привилегированный сеттер БЕЗ `hasRole`/guard → HIGH.
- (существующие `a1_setowner_open`, `ar1_role_open` — не должны регрессировать.)

## 6. Риски и границы

- **Над-подавление:** `isRoleQueryFn` слишком широк → примет функцию, читающую `mapping[param]` и не
  относящуюся к правам, как guard. Митигация: требуем, чтобы результат ГЕЙТИЛ revert И account-арг = msg.sender.
- **Short-circuit блоки (2.4):** может понадобиться доминатор-обход — держать в §4 шаг 6, не расширять слепо.
- **Cross-contract role-registration (createOffer)** — вне этого фикса (риск). Future.
- **Счётчики (`offerCount`)** — отдельный класс монотонного инкремента. Future.
- **Откат:** правка локализована в `isRoleQueryFn` + 2 passthrough-расширениях; при регрессе corpus08 —
  сузить `isRoleQueryFn` (требовать вложенный mapping ≥2 уровней, как у OZ) или откатить.
