# Неделя 4 на пальцах — тело функций

---

## Что мы теперь делаем

На прошлой неделе мы парсили «скелет» контракта: имена, параметры, типы возврата. Тело функции `{ ... }` пропускали — просто считали скобки. Теперь **залезаем внутрь**.

Вход:
```solidity
function increment(uint256 delta) public returns (uint256) {
    require(msg.sender == owner);
    uint256 newValue = value + delta;
    value = newValue;
    return newValue;
}
```

После недели 3 мы знали: «есть функция `increment`, у неё параметр `delta` типа uint256, возвращает uint256». Внутри — чёрный ящик.

После недели 4: каждая строчка внутри `{ }` — узел в нашем дереве. `require(...)`, `uint256 newValue = ...`, `value = ...`, `return ...` — четыре конкретные структуры в `FunctionDecl::body.stmts`.

---

## Два кита: statements и expressions

### Statements — действия

Это «команды», которые что-то делают:
- `return ...;` — вернуть значение
- `require(...);` — проверить условие, иначе откатиться
- `uint256 x = ...;` — объявить локальную переменную
- `x = ...;` — присвоить уже существующей
- `if (...) { ... } else { ... }` — ветвление

Каждый statement в нашем AST — это `struct` с теговым полем `kind`:
```cpp
struct ReturnStmt : Stmt {
    ExprPtr value;
};
```

### Expressions — значения

Это всё, что **вычисляется** в значение:
- Литералы: `42`, `true`, `false`
- Переменные: `x`, `value`
- Арифметика: `a + b`, `c * d`
- Сравнения: `x == y`, `a < b`
- Логика: `cond1 && cond2`, `!flag`
- Доступ к полю: `msg.sender`
- Вызовы: `require(...)`
- Скобки: `(a + b) * c`

Тоже `struct` с тегом:
```cpp
struct BinaryExpr : Expr {
    BinOp   op;     // например, Add
    ExprPtr lhs;    // левое значение
    ExprPtr rhs;    // правое
};
```

---

## Главная новая идея — приоритет операторов

Когда видишь `1 + 2 * 3`, ты автоматически понимаешь: сначала умножение, потом сложение. Результат — 7, а не 9. Парсер должен делать так же.

Это называется **приоритет** (precedence). Каждому оператору присваивается «уровень»:
- `||` — самый низкий (1)
- `&&` — выше (2)
- `==`, `!=` — ещё выше (3)
- `<`, `>` — 4
- `+`, `-` — 5
- `*`, `/` — самый высокий из бинарных (6)

Правило: при парсинге **сначала** обрабатывается оператор с более высоким приоритетом. То есть в `1 + 2 * 3` лексер выдаёт пять токенов:
```
Number(1) Plus Number(2) Star Number(3)
```
Парсер видит `+`, видит после него `*`. Поскольку `*` приоритетнее, парсер сначала собирает `(2 * 3)` в один Expr, а уже потом «отдаёт» его в `+`.

Результат — дерево:
```
BinaryExpr(Add)
├── lhs: NumberLit(1)
└── rhs: BinaryExpr(Mul)
        ├── lhs: NumberLit(2)
        └── rhs: NumberLit(3)
```

Этот алгоритм называется **precedence climbing** — «карабкаться по приоритетам». Очень короткий код (10 строк), и работает для всех бинарных операторов сразу.

### Ассоциативность

`1 - 2 - 3` — это `((1 - 2) - 3)` или `(1 - (2 - 3))`? Математически — первое, **левая ассоциативность**. Мы тоже так делаем.

Технически это решается одной строчкой:
```cpp
auto rhs = parseBinary(prec + 1);   // +1 → правая часть требует строго БОЛЬШЕГО приоритета
```
Если бы было `parseBinary(prec)` без `+1`, парсер собирал бы правоассоциативно. Этот трюк я подсмотрел в академических компиляторах — он стандартный.

---

## Постфикс: `.member` и `(args)`

Когда после идентификатора идёт `.` или `(`, это не «следующий токен», а **постфиксная операция** над предыдущим выражением.

Примеры:
- `foo.bar` → `MemberExpr(IdentExpr "foo", "bar")`
- `f()` → `CallExpr(IdentExpr "f", args=[])`
- `f(1, 2)` → `CallExpr(IdentExpr "f", args=[NumberLit "1", NumberLit "2"])`
- `msg.sender` → `MemberExpr(IdentExpr "msg", "sender")` ← **именно так мы будем добывать msg.sender**

В коде это обычный цикл после прочтения идентификатора:
```cpp
while (true) {
    if (match('.')) {
        // member access
    } else if (match('(')) {
        // call
    } else {
        break;
    }
}
```

Можно даже цепочки: `a.b.c` → `MemberExpr(MemberExpr(IdentExpr "a", "b"), "c")`.

---

## Skip-тела → реальный разбор

В неделе 3 в конце `parseFunctionDecl` стояло:
```cpp
skipBraceBody();      // ← считаем { и }, всё внутри игнорируем
```

На неделе 4 заменили на:
```cpp
parseBlockStatements(body.stmts);   // ← реально парсим каждый stmt
```

`parseBlockStatements`:
```cpp
1. ожидаем '{'
2. в цикле, пока не RBrace и не EOF:
   - parseStmt() — разбираем один statement
   - если получилось — кладём в out
   - если нет — recovery: пропустить до ';'
3. ожидаем '}'
```

Этот метод используется в **трёх местах**: тело функции, then-блок if, else-блок if. Один код, одно поведение.

---

## Что в результате

Запустим парсер на `counter.sol`:
```solidity
function increment(uint256 delta) public returns (uint256) {
    require(msg.sender == owner);
    uint256 newValue = value + delta;
    value = newValue;
    return newValue;
}
```

Парсер выдаст:
```
FunctionDecl "increment"
├── params: [Param(Uint256, "delta")]
├── returnType: Uint256
└── body.stmts:
    ├── [0] RequireStmt
    │   └── cond: BinaryExpr(Eq)
    │       ├── lhs: MemberExpr
    │       │   ├── base: IdentExpr("msg")
    │       │   └── member: "sender"
    │       └── rhs: IdentExpr("owner")
    ├── [1] VarDeclStmt(Uint256, "newValue")
    │   └── init: BinaryExpr(Add)
    │       ├── lhs: IdentExpr("value")
    │       └── rhs: IdentExpr("delta")
    ├── [2] AssignStmt
    │   ├── target: "value"
    │   └── value: IdentExpr("newValue")
    └── [3] ReturnStmt
        └── value: IdentExpr("newValue")
```

Каждое выражение, каждое сравнение, каждый `+` — теперь узел в нашем дереве.

---

## Маленькая хитрость с move-only

Когда мы добавили `std::vector<StmtPtr> stmts` в `Block`, появилась проблема: `unique_ptr` **нельзя копировать**. Цепочка:
- `Block` теперь нельзя копировать (vector от unique_ptr)
- `FunctionDecl body;` тоже нельзя копировать
- `ContractDecl::functions = std::vector<FunctionDecl>;` тоже работает только через `move`

В Parser.cpp была одна строка `fn.body = body;` (копирование). Её заменили на `fn.body = std::move(body);`. После `move` оригинал `body` пуст — но он нам больше и не нужен, мы его сразу же возвращаем.

Эта дисциплина «всё передаём через `std::move`, ничего не копируем» — стандарт современного C++. Когда деревья большие, копирование было бы безумно дорогим.

---

## Pitfall, в который мы могли попасть

После замены `skipBraceBody` на `parseBlockStatements` сломался **один** существующий тест — `Parser.UnterminatedFunctionBodyProducesDiag`. Раньше диагностику «unterminated `{` body» писал `skipBraceBody`. Теперь его не зовут для тел функций, и диагностика терялась.

Subagent сам заметил это, добавил такую же диагностику в `parseBlockStatements` — теперь у нас единая точка сообщения. `skipBraceBody` остался для error-recovery (`synchronizeToBraceEnd`), но без диагностики (там она была бы дубликатом).

Это пример **cross-cut**: одно изменение трогает несколько мест. Хорошо что у нас на каждое поведение есть тест — без него такая регрессия проскользнула бы.

---

## Что теперь умеет компилятор

`counter.sol` теперь полностью разбирается в AST. 58 тестов зелёные, включая полную интеграцию с фикстурой.

Но пока компилятор **не понимает смысла** того, что разобрал. Он не знает, что `value` — это переменная контракта, а `delta` — параметр. Он не проверяет, что `value + delta` — это сложение двух `uint256` (а не, скажем, `bool + address`).

Этим займётся неделя 5 — **семантический анализ**.

---

## Что дальше — неделя 5

«Идентификатор `value`» должен превратиться в «ссылка на state variable `value` в слоте 0 типа uint256». Это работа **семантика**:
- Symbol table — словарь «имя → что это» (state var, параметр, локальная, builtin)
- Scope stack — вложенные области видимости (глобальный → функция → блок)
- Type checking — проверка, что `+` применяется к двум uint256, а не к bool
- Builtins — `msg.sender` (address), `msg.value` (uint256), `require(bool)`

После недели 5 AST будет «обогащённым» — каждый IdentExpr будет знать, на что он ссылается, каждое выражение — свой тип.

---

## Краткое резюме

- Statements — действия (`return`, `require`, `=`, `if`).
- Expressions — значения (литералы, ident, арифметика, сравнения).
- Precedence climbing — алгоритм правильного приоритета операторов.
- Постфикс `.member` и `(args)` — для `msg.sender` и `require(...)`.
- 58 тестов, полный `counter.sol` end-to-end.
- На неделе 5 — семантика (типы и имена).

Парсер закончен. Полпути курсовой пройдено.
