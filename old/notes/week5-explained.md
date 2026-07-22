# Неделя 5 на пальцах — семантика

---

## Что такое семантический анализ

Парсер построил **дерево** из текста. Но он не понял **смысл**: что значит `value`, какого типа `delta + value`, корректно ли `bool b = 1`.

Семантика — это этап, который **проверяет смысл**:
- Каждое имя должно ссылаться на что-то реально объявленное.
- Каждое выражение должно иметь корректный тип.
- Каждое присваивание, return, require — должны типово согласовываться.
- Все state vars получают конкретное **место в storage** (storageSlot 0, 1, 2, …).

После семантики AST становится **обогащённым**: на каждом узле висит метка типа, на каждом идентификаторе — метка «откуда это» (state var? параметр? локальная?).

---

## Что мы сделали на этой неделе

### Symbol table — словарь имён

`SymbolTable` — структура «как стек словарей». Каждый scope (контракт, функция, then-блок, else-блок) — отдельный словарь имя → информация.

```cpp
class SymbolTable {
    std::vector<Scope> stack_;   // стек словарей
};
```

Когда заходим в новый scope (например, тело функции), пушим новый пустой словарь. Когда выходим — попаем. Если объявляем `uint256 x = 1;` внутри функции — кладём `x` в верхний словарь стека. Если ищем `value` — идём сверху вниз: сначала проверяем текущий словарь, потом родительский, потом ещё выше.

Внутри одного scope нельзя объявить два `x` — `declare` вернёт false. Это и есть проверка на дубликаты.

### Что хранит Symbol

```cpp
struct Symbol {
    SymbolKind   kind;       // StateVar / Param / Local / Function
    std::string  name;
    ast::Type    type;       // uint256 / bool / address
    int          storageSlot;// для StateVar — 0, 1, 2... иначе -1
    SourceLoc    declLoc;    // где объявлено — для красивых диагностик
};
```

### Сам анализатор — `SemanticAnalyzer`

Главный метод:
```cpp
bool analyze(ContractDecl& contract);
```
Возвращает `true` если ошибок нет. Модифицирует AST — заполняет аннотации.

Алгоритм:
1. Enter global scope.
2. Зарегистрировать все state vars (присвоить storageSlot 0, 1, 2, …).
3. Зарегистрировать все functions.
4. Для каждой функции:
   - Enter function scope, declare params.
   - Пройтись по `body.stmts` вызывая `checkStmt`.
   - Leave scope.
5. Leave global scope.
6. Вернуть `ok && !hasErrors()`.

---

## checkExpr — рекурсивная проверка выражений

Это «сердце» семантики. Каждый Expr получает `resolvedType` — определённый тип.

Пример: для `1 + 2 * 3`:
- Слева `1` → NumberLit → `Uint256`
- Справа `2 * 3` → рекурсивно: `2` → `Uint256`, `3` → `Uint256`, `Mul` оба `Uint256` → результат `Uint256`
- `Add` слева `Uint256` справа `Uint256` → результат `Uint256` ✓

Если бы было `true + 1`:
- `true` → `Bool`
- `1` → `Uint256`
- `Add` ждёт оба `Uint256`, получил `Bool + Uint256` → **diag «operator '+' expects uint256»** и false

### Резолвинг идентификаторов

`IdentExpr("value")`:
1. Спросить SymbolTable: что такое `value`?
2. Найти Symbol с `kind=StateVar, storageSlot=0, type=Uint256`.
3. Скопировать данные в AST: `id.resolvedKind = Resolution::StateVar`, `id.resolvedSlot = 0`, `id.resolvedType = Uint256`.

Кодегенератор на следующей неделе посмотрит на эти поля и поймёт: «надо позвать `evm.sload(slot=0)`».

### msg.sender и msg.value — фейковые builtins

Это специальные имена, у которых **нет настоящего Symbol**. Мы их обрабатываем вручную в `checkExpr(MemberExpr)`:

```cpp
if (m.base — IdentExpr с именем "msg") {
    if (member == "sender") → resolvedType = Address; return true;
    if (member == "value")  → resolvedType = Uint256; return true;
    else → diag "unknown member 'msg.<x>'"
}
```

Никакого «msg» в SymbolTable нет — иначе lookup бы выдал «undeclared identifier 'msg'».

---

## Главное архитектурное решение недели — by value, не по указателю

Изначально план хранил в `IdentExpr` указатель `sema::Symbol*` — мол, «ссылка на тот символ в таблице». Плановый ревьюер заметил баг:

> «Когда `leaveScope()` попает scope из стека — все Symbol в этом scope **удаляются**. Указатели в AST становятся dangling. Codegen на неделе 6 их попытается прочитать и получит UB.»

Это не мысленный риск — это **гарантированный sigsegv** на неделе 6. Хорошо, что поймали в плане, а не в коде.

Решение: **копировать нужные данные** прямо в AST по значению:

```cpp
struct IdentExpr : Expr {
    std::string name;
    enum class Resolution { Unresolved, StateVar, Param, Local, Function };
    Resolution resolvedKind = Unresolved;
    int        resolvedSlot = -1;
};
```

Получили: AST после семы **самодостаточен**, SymbolTable можно безопасно уничтожить. Цена — каждая `IdentExpr` потратит лишних ~8 байт. Копейки.

---

## Что теперь умеет компилятор

После недели 5 на `counter.sol` пайплайн lexer → parser → sema:
- Возвращает `true` (всё ок)
- 0 диагностик
- `value` имеет `storageSlot=0`, `owner` — `storageSlot=1`
- Каждое выражение в обоих функциях типизировано
- Каждый `IdentExpr` знает, на что он ссылается

Можно прогнать (только в тестах пока, в CLI sema подключим на неделе 8):
```bash
cd ~/diplom/solidity-llvm/build
ctest --output-on-failure
```
Увидишь **80/80 passed**, включая `Sema.CounterFixturePassesSema`.

---

## Пример: что внутри AST после sema

Для строки `value = value + delta;` в `increment`:

```
AssignStmt
├── target: "value"
└── value: BinaryExpr(Add, resolvedType=Uint256)
        ├── lhs: IdentExpr("value")
        │   ├── resolvedKind: StateVar
        │   ├── resolvedSlot: 0          ← state var в слоте 0!
        │   └── resolvedType: Uint256
        └── rhs: IdentExpr("delta")
            ├── resolvedKind: Param
            ├── resolvedSlot: -1          ← не state var
            └── resolvedType: Uint256
```

Кодегенератор всё про каждый узел знает:
- `value` — это state var в слоте 0 → `sload(0)`
- `delta` — параметр функции → читай из `alloca` параметра
- `Add` обоих `Uint256` → LLVM `add i256`
- Запиши результат обратно в `value` → `sstore(0, ...)`

---

## Что было лёгкого/неприятного

**Лёгкое:**
- Symbol table — стандартная штука, 20 строк кода.
- Литералы и идентификаторы — буквально 3 кейса свитча.
- msg.sender — спец-кейс на 5 строк.

**Поинтереснее:**
- Не наступить на тип-mismatch при VarDecl: сначала проверить, потом объявить (иначе словарь засрётся плохими записями).
- В If — два **отдельных** scope'а для then и else (a в else можно объявить var с тем же именем, что в then — это разные scopes).
- Default-ветка свитча. Все StmtKind в итоге обработаны явно, но default оставлен на случай добавления новых kind'ов. Так компилятор не упадёт молча.

**Сложное (но решено):**
- Lifetime аннотаций. Хранение по значению — единственное правильное решение для нашего случая.

---

## Что дальше — неделя 6

**Codegen.** Самая интересная часть. Берём обогащённый AST и через `llvm::IRBuilder` строим **настоящий LLVM IR**:
- Solidity типы → LLVM: `uint256 → i256`, `bool → i1`, `address → i160`
- State vars читаются через intrinsic `@evm.sload(i256 slot)`, пишутся через `@evm.sstore(i256 slot, i256 value)`
- Локальные — через LLVM `alloca` + `load`/`store`
- Арифметика — обычные LLVM `add`, `sub`, `mul`, `udiv`
- Сравнения — `icmp eq`/`ult`/...
- `if/else` — `br`/`phi`
- `return` — `ret`

После недели 6 — первый валидный `.ll` для Counter, проходящий `llvm-as` (без `require` и `msg.*`, это неделя 7).

---

## Краткое резюме

- Symbol table + scope stack: словарь имён по уровням.
- checkExpr типизирует каждое выражение.
- AST после семы знает про каждый идентификатор: state var? параметр? локальная? слот? тип?
- Главное решение: **аннотации хранятся по значению**, иначе указатели бы dangle'или.
- 80/80 тестов, counter.sol чистый.
- Дальше — codegen в LLVM IR.

Семантика — последний фронтенд-этап. Со следующей недели начинается реальная компиляция.
