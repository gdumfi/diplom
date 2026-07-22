# Полный разбор кода `solidity-llvm` — для подготовки к защите

Этот файл — конспект **всего реального кода** проекта, чтобы можно было ответить
на любой вопрос «а что тут происходит в этой функции». Читать вместе с
исходниками: пути указаны как `файл:строка`.

Всего ~1870 строк C++ в `src/` + `include/solc/`, разбито на 7 подсистем.

---

## 0. Карта проекта

```
include/solc/                            src/
├── diag/Diagnostic.h     (29)           ├── diag/Diagnostic.cpp     (9)
├── lexer/Token.h         (38)           ├── lexer/Lexer.cpp         (158)
├── lexer/Lexer.h         (31)           ├── ast/AST.cpp             (61)
├── ast/AST.h             (184)          ├── parser/Parser.cpp       (451)
├── parser/Parser.h       (56)           ├── sema/Symbol.cpp         (33)
├── sema/Symbol.h         (41)           ├── sema/Sema.cpp           (267)
├── sema/Sema.h           (25)           ├── codegen/EvmIntrinsics.cpp (26)
├── codegen/EvmIntrinsics.h (14)         ├── codegen/CodeGen.cpp     (266)
├── codegen/CodeGen.h     (46)           ├── driver/Driver.cpp       (80)
└── driver/Driver.h       (17)           └── main.cpp                (37)
```

Один контракт `.sol` идёт по конвейеру:

```
текст файла
    │
    ▼ Lexer.next()           → выдаёт Token-ы по требованию
    ▼ Parser.parseContract() → строит ContractDecl (AST)
    ▼ SemanticAnalyzer.analyze() → проверяет типы, имена, проставляет аннотации
    ▼ CodeGen.generate()     → отдаёт llvm::Module
    ▼ Driver печатает мод-> в stdout или файл
.ll-текст
```

Всё это сшито в [src/driver/Driver.cpp:26-78](../solidity-llvm/src/driver/Driver.cpp#L26-L78).

---

## 1. `Diagnostic` — общий канал ошибок

[include/solc/diag/Diagnostic.h](../solidity-llvm/include/solc/diag/Diagnostic.h)

```cpp
struct SourceLoc { uint32_t line = 1, col = 1; };   // 1-based
struct Diagnostic { SourceLoc loc; std::string message; };
class DiagnosticEngine {
    void error(SourceLoc, std::string);
    const std::vector<Diagnostic>& errors() const;
    bool hasErrors() const;
};
```

**Зачем нужен.** Все этапы (лексер, парсер, sema, codegen) принимают один и тот
же `DiagnosticEngine&` и пишут туда ошибки. Это плоский вектор, без иерархии. В
конце пайплайна Driver вызывает `printDiagnostics()` и выкатывает их в `stderr`
в формате `error: <line>:<col>: <message>`.

**Что отвечать на вопрос «почему не исключения?»**
> «Компилятор должен сообщать о **множестве** ошибок за один проход, а не падать
> на первой. Накопительный буфер — стандартный паттерн (clang, gcc, rustc).
> Исключения здесь были бы абортом всего процесса, а пользователю важно увидеть
> все проблемы сразу.»

---

## 2. Лексер

### 2.1. `Token` и `TokenKind`

[include/solc/lexer/Token.h](../solidity-llvm/include/solc/lexer/Token.h)

```cpp
enum class TokenKind { Identifier, Number, True, False,
                       KwUint256, KwBool, KwAddress,
                       KwContract, KwFunction, KwPublic, KwReturns, KwReturn,
                       KwIf, KwElse, KwRequire,
                       Plus, Minus, Star, Slash,
                       Eq, EqEq, NotEq, Lt, Gt, LtEq, GtEq,
                       AndAnd, OrOr, Not,
                       LParen, RParen, LBrace, RBrace, Comma, Semicolon, Dot,
                       EndOfFile, Invalid };

struct Token { TokenKind kind; std::string_view lexeme; SourceLoc loc; };
```

**Ключевое:** `lexeme` — это `std::string_view`, **указатель в исходник без
копирования**. Текст файла живёт всю компиляцию в одной строке, все Token-ы —
это её «срезы». Это экономит память и аллокации.

**Сколько токенов:** 35 значащих + `EndOfFile`/`Invalid` = 37. Минимум,
который покрывает MVP подмножества.

### 2.2. `Lexer` — алгоритм

[src/lexer/Lexer.cpp](../solidity-llvm/src/lexer/Lexer.cpp)

Это **однопроходный лексер на конечном автомате** (FSM, finite state machine),
написан вручную. Не использует `flex`/`ragel`. Главный метод — `Token next()`,
который выдаёт **следующий** токен по требованию (так называемый pull-based
lexer; парсер дёргает его в нужный момент).

Внутреннее состояние:
- `src_` — `std::string_view` на исходник
- `pos_` — текущая позиция в байтах
- `loc_` — текущая строка/столбец для красивых ошибок

#### `peek(offset)` и `advance()` — базовые шаги

[Lexer.cpp:24-33](../solidity-llvm/src/lexer/Lexer.cpp#L24-L33)
```cpp
char Lexer::peek(size_t offset) const {
    return pos_ + offset < src_.size() ? src_[pos_ + offset] : '\0';
}

char Lexer::advance() {
    char c = src_[pos_++];
    if (c == '\n') { loc_.line++; loc_.col = 1; }   // перевод строки
    else           { loc_.col++; }
    return c;
}
```

`peek` смотрит без потребления, `advance` потребляет один символ и обновляет
координаты. На `\0` (EOF) `peek` возвращает безопасный нуль — не вылезаем за
конец строки.

#### `skipWhitespaceAndComments()`

[Lexer.cpp:41-62](../solidity-llvm/src/lexer/Lexer.cpp#L41-L62)

Цикл, пока следующий символ — пробел/таб/CR/LF — потребляем. `//` — построчный
комментарий (до `\n`). `/* ... */` — блочный. Если блочный незакрыт до EOF —
**ошибка** в диагностики.

#### Основной диспетчер `next()`

[Lexer.cpp:68-118](../solidity-llvm/src/lexer/Lexer.cpp#L68-L118)

Логика:
1. скипнуть пробелы/комменты
2. если EOF — выдать `EndOfFile`
3. посмотреть первый символ:
   - буква или `_` → идём в `lexIdentifierOrKeyword`
   - цифра → идём в `lexNumber`
   - однострочные операторы/пунктуация (`(`, `)`, `{`, …, `+`, `-`, `*`, `/`)
     → выдать сразу
   - **двухсимвольные** (`==`, `!=`, `<=`, `>=`, `&&`, `||`) → смотрим
     второй символ через `match(c)`. Если совпало — потребляем и выдаём
     двухсимвольный токен; иначе односимвольный.
   - `&` или `|` без пары — **ошибка** «unexpected char».

> Важная нюанс: одиночные `&` или `|` — невалидны в нашем подмножестве. Мы их
> явно выкидываем диагностикой. В диплом расширим под битовые операции.

#### `lexIdentifierOrKeyword`

[Lexer.cpp:120-146](../solidity-llvm/src/lexer/Lexer.cpp#L120-L146)

Читаем `[_a-zA-Z0-9]+`, потом смотрим в **таблицу ключевых слов** (compile-time
`constexpr` массив). Если совпало — отдаём соответствующий keyword-token;
иначе `Identifier`. 13 ключевых слов всего.

> Линейный поиск по 13 элементам — быстрее, чем `std::unordered_map`, потому
> что хеш-таблица для маленьких N (≤16) проигрывает прямому сравнению.

#### `lexNumber`

[Lexer.cpp:148-156](../solidity-llvm/src/lexer/Lexer.cpp#L148-L156)

Читаем цифры. Если число начинается с `0` и длиннее 1 символа — **ошибка**
(«leading zero is not allowed»). Это требование Solidity 0.5+: `007` запрещено,
потому что когда-то парсилось как восьмеричное и было источником багов.

### Что отвечать на типичные вопросы по лексеру

> «Почему лексер ручной, а не bison/flex?» — Грамматика маленькая (~13
> keyword’ов), ручной FSM компактнее и даёт лучшие сообщения об ошибках. Так же
> делает clang.

> «Что такое lookahead?» — Возможность посмотреть на N символов вперёд **без
> потребления**. У меня lookahead 1: `peek()` без аргумента, и `peek(1)` для
> двухсимвольных лексем (`//`, `/*`, `==`).

> «Откуда `i256` в числовых литералах?» — В лексере число — это просто строка
> текста (`std::string`, чтобы пережить `string_view` на этапах после
> AST-передачи). Парсинг в LLVM-константу — уже в codegen через
> `llvm::APInt(256, text, 10)`.

---

## 3. AST — структура дерева

[include/solc/ast/AST.h](../solidity-llvm/include/solc/ast/AST.h)

AST = Abstract Syntax Tree, дерево, в которое парсер «упаковывает» программу.

### 3.1. Базовые узлы

```cpp
enum class TypeKind { Uint256, Bool, Address };       // 3 типа всего
struct Type { TypeKind kind; SourceLoc loc; };
```

Дальше — два больших семейства: **Expr** (выражения) и **Stmt** (statement’ы).
И тот, и другой — полиморфные базовые классы с `virtual ~`. Конкретные узлы —
наследники.

### 3.2. Выражения

```cpp
enum class ExprKind { NumberLit, BoolLit, Ident, Binary, Unary, Member, Call };

struct Expr {
    ExprKind kind;
    SourceLoc loc;
    std::optional<Type> resolvedType;   // ← Sema проставит после анализа
    virtual ~Expr() = default;
};
using ExprPtr = std::unique_ptr<Expr>;
```

**Ключевая идея**: `resolvedType` — это **аннотация**, которую дописывает
семантика. Парсер её оставляет пустой. CodeGen на неё опирается.

#### `IdentExpr` — с пристёгнутой Resolution

```cpp
struct IdentExpr : Expr {
    std::string name;
    enum class Resolution { Unresolved, StateVar, Param, Local, Function };
    Resolution resolvedKind = Resolution::Unresolved;
    int        resolvedSlot = -1;   // storageSlot для StateVar; иначе -1
    // ...
};
```

После Sema идентификатор знает **что он за зверь**: state-переменная (тогда у
него есть slot 0/1/…), параметр, локалка или функция. CodeGen уже видит готовый
ответ и просто эмитит нужные инструкции.

#### `BinaryExpr` / `UnaryExpr`

```cpp
enum class BinOp { Add, Sub, Mul, Div, Eq, NotEq, Lt, Gt, LtEq, GtEq, AndAnd, OrOr };
enum class UnOp  { Not };
```

12 бинарных + 1 унарный (`!`). Унарного минуса нет — он не нужен для `uint256`,
а для bool используется `!`.

#### `MemberExpr` и `CallExpr`

`MemberExpr` — это `a.b` (нам нужно для `msg.sender`/`msg.value`).
`CallExpr` — это `f(arg1, arg2)`. Используется только при разборе `require(...)`,
но в MVP Sema не разрешает Call как expression — `require` обрабатывается отдельно.

### 3.3. Statements

```cpp
enum class StmtKind { VarDecl, Assign, If, Return, Require };
```

- `VarDeclStmt` — `uint256 x = expr;`
- `AssignStmt`  — `x = expr;` (имя target — просто строка)
- `IfStmt`      — `if (cond) { ... } else { ... }`, обе ветки — вектора
- `ReturnStmt`  — `return expr;`
- `RequireStmt` — `require(cond);` (специальный statement, а не вызов функции)

### 3.4. Декларации верхнего уровня

```cpp
struct StateVarDecl { Type type; std::string name; bool isPublic;
                      int storageSlot = -1; SourceLoc loc; };
struct Param        { Type type; std::string name; SourceLoc loc; };
struct FunctionDecl { std::string name; std::vector<Param> params;
                      std::optional<Type> returnType; Block body; SourceLoc loc; };
struct ContractDecl { std::string name; std::vector<StateVarDecl> stateVars;
                      std::vector<FunctionDecl> functions; SourceLoc loc; };
```

**`storageSlot = -1`** до Sema (не разрешён), потом она проставит 0, 1, 2 …
**`returnType` — `std::optional<Type>`** потому что функция может быть `void`
(`returns (...)` опциональный).

---

## 4. Парсер

[src/parser/Parser.cpp](../solidity-llvm/src/parser/Parser.cpp)

**Техника:** рекурсивный спуск с **precedence climbing** (для выражений).

### 4.1. Базовый API

[Parser.cpp:7-32](../solidity-llvm/src/parser/Parser.cpp#L7-L32)

```cpp
const Token& peek() const;                  // текущий токен (lookahead 1)
Token        advance();                     // потребить, вернуть потреблённый
bool         check(TokenKind k);            // peek().kind == k?
bool         match(TokenKind k);            // если check — потребить и true
bool         expect(TokenKind k, ctx);      // если check — ok; иначе diag
```

Состояние:
- `current_` — кэш «следующего» токена (один токен lookahead). Когда вызываем
  `advance()`, он становится потреблённым, а в `current_` записывается следующий
  из лексера.

`expect` — это диагностический helper: если ожидаемого токена нет, печатает
`expected X in Y, got Z` и возвращает `false`. После этого вызывающий обычно
зовёт `synchronize…` для error recovery.

### 4.2. Error recovery

[Parser.cpp:34-49](../solidity-llvm/src/parser/Parser.cpp#L34-L49)

Это **критическая часть** — без неё парсер падает на первой ошибке.

```cpp
void synchronizeToSemicolon();   // пропустить токены до ';' или конца блока/файла
void synchronizeToBraceEnd();    // пропустить до '{' / '}', '{' разбалансировать
```

Идея простая: после ошибки **выкинуть всё до следующего safe-point** (точка с
запятой или конец блока), чтобы продолжить парсить как ни в чём не бывало. Это
то, что позволяет компилятору выдать **много ошибок** за один запуск.

### 4.3. Декларации

`parseContract()` ([Parser.cpp:193-228](../solidity-llvm/src/parser/Parser.cpp#L193-L228)):
```
contract Name {
    <members>...
}
```
В цикле смотрит **первый токен** каждого member’а:
- `function` → `parseFunctionDecl()`
- иначе → `parseStateVarDecl()`

Это решение по lookahead 1 без backtracking — нам этого хватает, потому что
state-var всегда начинается с типа (`uint256`/`bool`/`address`), а функция — с
keyword `function`.

`parseStateVarDecl()` ([Parser.cpp:64-86](../solidity-llvm/src/parser/Parser.cpp#L64-L86)):
```
type ['public'] ident ';'
```

`parseFunctionDecl()` ([Parser.cpp:120-191](../solidity-llvm/src/parser/Parser.cpp#L120-L191)):
```
'function' ident '(' params? ')' 'public' ['returns' '(' type ')'] block
```
- `public` **обязателен** в нашем MVP (нет других visibility-модификаторов)
- `returns (T)` — опционально

### 4.4. Statements

`parseStmt()` ([Parser.cpp:375-449](../solidity-llvm/src/parser/Parser.cpp#L375-L449)) — большой `if`-каскад по первому токену:

| Первый токен | Что парсим |
|---|---|
| `if`      | `IfStmt` |
| `return`  | `ReturnStmt` |
| `require` | `RequireStmt` |
| `uint256`/`bool`/`address` | `VarDeclStmt` |
| `Identifier` | `AssignStmt` |
| иначе | ошибка |

Тут важная неоднозначность: identifier в начале statement — это **присваивание**,
а не выражение. У нас нет expression-statements (вызов функции как statement)
— парсер сразу ожидает `=` после идентификатора. В дипломе добавим внешние
вызовы — там придётся переделать.

### 4.5. Выражения — precedence climbing

Это самая интересная часть парсера.

#### Таблица приоритетов

[Parser.cpp:291-302](../solidity-llvm/src/parser/Parser.cpp#L291-L302)
```cpp
int Parser::binOpPrecedence(TokenKind k) {
    switch (k) {
        case OrOr:                              return 1;  // ||
        case AndAnd:                            return 2;  // &&
        case EqEq: case NotEq:                  return 3;  // == !=
        case Lt: case Gt: case LtEq: case GtEq: return 4;  // < > <= >=
        case Plus: case Minus:                  return 5;  // + -
        case Star: case Slash:                  return 6;  // * /
        default:                                return 0;
    }
}
```

Чем **выше** число — тем **сильнее** оператор «склеивает». `*` сильнее `+`,
`+` сильнее сравнения, сравнение сильнее `&&`, `&&` сильнее `||`.

#### Алгоритм

```cpp
ExprPtr parseExpr()             { return parseBinary(1); }
ExprPtr parseUnary()            { /* '!' или parsePrimary */ }
ExprPtr parseBinary(int minPrec) {
    auto lhs = parseUnary();
    while (prec(peek()) >= minPrec) {
        op = peek(); advance();
        auto rhs = parseBinary(prec(op) + 1);   // ← +1 = левая ассоциативность
        lhs = Binary(op, lhs, rhs);
    }
    return lhs;
}
```

Это **классический Pratt/precedence climbing**. Пример:
```
1 + 2 * 3
```
1. `parseBinary(1)` → `lhs = 1`
2. `peek = '+', prec(+) = 5 ≥ 1` → ok
3. потребляем `+`, рекурсивно зовём `parseBinary(6)`
4. внутри: `lhs = 2`, `peek = '*', prec(*) = 6 ≥ 6` → ok
5. потребляем `*`, рекурсивно зовём `parseBinary(7)`
6. внутри: `lhs = 3`, дальше нет операторов с prec ≥ 7 → возвращаем 3
7. возвращаемся на шаг 5: `rhs = 3` → возвращаем `Binary(*, 2, 3)`
8. возвращаемся на шаг 2: `rhs = (2 * 3)` → возвращаем `Binary(+, 1, (2 * 3))`

В итоге дерево — `1 + (2 * 3)`. Приоритет уважён.

**Почему `prec(op) + 1`?** Это даёт **левую ассоциативность**. `a - b - c`
парсится как `(a - b) - c`, а не `a - (b - c)`. Если бы был `prec(op)` без +1
— была бы правая.

#### `parsePrimary()`

[Parser.cpp:230-289](../solidity-llvm/src/parser/Parser.cpp#L230-L289)

Атомарные выражения:
- `Number` → `NumberLit`
- `true`/`false` → `BoolLit`
- `Identifier` → `IdentExpr`, потом **постфиксный цикл**:
  - `.name` → оборачиваем в `MemberExpr` (это нужно для `msg.sender`)
  - `(args, ...)` → оборачиваем в `CallExpr` (это нужно для `require(...)`)
  - можно цеплять много раз (теоретически — `a.b.c(x)(y).d` хоть и не
    встречается в MVP)
- `(expr)` — скобки убираем, отдаём внутреннее выражение

#### Что отвечать про парсер

> «Почему рекурсивный спуск?» — Грамматика подмножества **LL(1)** (lookahead 1
> хватает для решения по первому токену). Рекурсивный спуск — самая прямая и
> читаемая реализация LL(k) парсеров. Clang тоже так написан.

> «Какая ассоциативность у `=`?» — Присваивание не часть выражений в нашем
> подмножестве, это **statement**. Поэтому `a = b = c` синтаксически невозможен.

> «Как поддерживается унарный `-`?» — Не поддерживается. `uint256` беззнаковый,
> отрицательных литералов нет. Унарный `!` есть, реализован в `parseUnary()`.

---

## 5. Семантика

### 5.1. `SymbolTable` — стек скоупов

[include/solc/sema/Symbol.h](../solidity-llvm/include/solc/sema/Symbol.h) + [src/sema/Symbol.cpp](../solidity-llvm/src/sema/Symbol.cpp)

```cpp
struct Symbol {
    SymbolKind   kind;        // StateVar / Param / Local / Function
    std::string  name;
    ast::Type    type;
    int          storageSlot; // только для StateVar
    SourceLoc    declLoc;
};
class Scope { unordered_map<string, Symbol> table_; };
class SymbolTable {
    std::vector<Scope> stack_;   // стек словарей
};
```

**Операции:**
- `enterScope()` — `stack_.emplace_back()` (новый пустой словарь сверху)
- `leaveScope()` — `pop_back`
- `declare(s)` — добавить в **верхний** словарь; вернёт `false`, если в этом
  скоупе уже есть с таким именем
- `lookup(name)` — пройти **сверху вниз**: ищем в текущем скоупе, не нашли —
  в родительском, и т.д.

**Какие скоупы есть в нашем коде:**
1. **Глобальный (контракт)** — state-vars + functions
2. **Функция** — параметры + локалки
3. **`then`-блок if** — отдельный скоуп (локалка в `then` не видна в `else` и
   после `if`)
4. **`else`-блок if** — отдельный скоуп

> «Почему `unordered_map<string, Symbol>`, а не linear?» — Имена могут
> повторяться в разных функциях, и поиск по имени должен быть O(1). Локальные
> скоупы маленькие (5-10 имён), но единообразно лучше иметь O(1) везде.

### 5.2. `SemanticAnalyzer` — основной проход

[src/sema/Sema.cpp](../solidity-llvm/src/sema/Sema.cpp)

#### `analyze(ContractDecl&)` — внешний цикл

[Sema.cpp:257-265](../solidity-llvm/src/sema/Sema.cpp#L257-L265)
```cpp
bool analyze(ContractDecl& c) {
    symbols_.enterScope();              // 1. глобальный скоуп
    registerContractMembers(c);         // 2. зарегать state-vars + functions
    for (auto& fn : c.functions)
        analyzeFunction(fn);            // 3. проверить тело каждой функции
    symbols_.leaveScope();
    return !diag_.hasErrors();
}
```

#### `registerContractMembers` — назначение storage слотов

[Sema.cpp:13-35](../solidity-llvm/src/sema/Sema.cpp#L13-L35)

Главная вещь: **проходимся по state-vars в порядке объявления и назначаем им
storageSlot 0, 1, 2, …**. Это то же, что делает официальный Solidity:

```solidity
contract C {
    uint256 value;     // slot 0
    address owner;     // slot 1
}
```

Также сразу регистрируем все функции в глобальном скоупе. Дубликаты — ошибка.

> **Почему сначала зарегать все members, потом анализировать тела?** — Чтобы
> функции могли ссылаться друг на друга и на state-vars **независимо от
> порядка объявления**. Это «two-pass» подход: сначала forward-объявления,
> потом тела.

#### `analyzeFunction` — проверка тела

[Sema.cpp:238-255](../solidity-llvm/src/sema/Sema.cpp#L238-L255)

```cpp
symbols_.enterScope();                  // скоуп функции
for (param : fn.params) declare(Param);
currentReturnType_ = fn.returnType ? &*fn.returnType : nullptr;
for (stmt : fn.body) checkStmt(*stmt);
currentReturnType_ = nullptr;
symbols_.leaveScope();
```

`currentReturnType_` — это указатель на ожидаемый тип возврата текущей функции,
чтобы `checkStmt(ReturnStmt)` мог его проверить. Нулевой = функция `void`.

#### `checkExpr` — основная type-checking логика

[Sema.cpp:37-154](../solidity-llvm/src/sema/Sema.cpp#L37-L154)

Снизу вверх по дереву выражений. Для каждого узла:
1. рекурсивно проверить дочерние выражения (они дозаполнят свой `resolvedType`)
2. вывести тип текущего узла по правилам
3. записать `e.resolvedType`

**Правила (важные):**

| Оператор | Что ожидает | Что возвращает |
|---|---|---|
| `+ - * /` | оба `uint256` | `uint256` |
| `< > <= >=` | оба `uint256` | **bool** |
| `== !=` | оба того же типа | **bool** |
| `&& \|\|` | оба `bool` | `bool` |
| `!` | `bool` | `bool` |

Замечу важный момент в `Eq`/`NotEq`: можно сравнивать **любые два значения
одного типа** — `address == address`, `uint256 == uint256`, `bool == bool`. Но
**нельзя** `address == uint256`. Это то, что в Solidity называется «strict
type equality for comparisons».

**Идентификаторы** ([Sema.cpp:46-64](../solidity-llvm/src/sema/Sema.cpp#L46-L64)):
```cpp
Symbol* sym = symbols_.lookup(id.name);
if (!sym) error("undeclared identifier");
// копируем нужное в AST по значению — указатели на Symbol небезопасны
// после leaveScope(), а codegen на неделе 6 это читает
id.resolvedKind = ...;     // Local / Param / StateVar / Function
id.resolvedSlot = sym->storageSlot;
id.resolvedType = sym->type;
```

Вот это **ключевая инженерная вещь**: мы не сохраняем `Symbol*`, а
**копируем** нужные поля в AST-узел. Почему? Когда `leaveScope()` происходит,
`Scope` уничтожается вместе со своими `Symbol`’ами. Если бы codegen хранил
указатели — был бы dangling pointer. А значения в AST живут весь компилятор.

**Спец-кейс `msg.sender`/`msg.value`** ([Sema.cpp:122-143](../solidity-llvm/src/sema/Sema.cpp#L122-L143)):
```cpp
case ExprKind::Member: {
    auto& m = static_cast<MemberExpr&>(e);
    if (m.base->kind == ExprKind::Ident) {
        auto& base = static_cast<IdentExpr&>(*m.base);
        if (base.name == "msg") {
            if (m.member == "sender") { resolvedType = Address; return true; }
            if (m.member == "value")  { resolvedType = Uint256; return true; }
            error("unknown msg.X");
        }
    }
    error("member access not supported");
}
```

`msg.sender` и `msg.value` — это builtin’ы. У `msg` нет настоящего Symbol —
sema распознаёт его **по имени**. Тип результата фиксирован: sender → address
(160 бит), value → uint256.

#### `checkStmt`

[Sema.cpp:156-236](../solidity-llvm/src/sema/Sema.cpp#L156-L236)

- **`VarDecl`**: проверить инициализатор, сверить тип, объявить локалку в
  текущем скоупе.
- **`Assign`**: найти target, проверить тип RHS, сверить.
- **`Require`**: внутри должно быть `bool`-выражение.
- **`If`**: cond обязан быть `bool`. **Каждый блок — отдельный новый скоуп**
  (enterScope/leaveScope вокруг then и else).
- **`Return`**: значение должно совпадать с `currentReturnType_`. Возврат без
  значения в non-void функции — ошибка.

> «А проверяет ли семантика, что **все пути** функции делают return?»
> — Нет, не проверяет. CodeGen имеет safety-net в `emitFunctionBody`: если в
> конце нет terminator’а, добавляет `ret <zero>` или `ret void`. Это можно
> считать недосказанным MVP, но `verifyModule` всё равно проверит, что IR
> валиден. Полноценный «definite return analysis» — задача диплома.

---

## 6. CodeGen — генерация LLVM IR

### 6.1. EVM intrinsics

[src/codegen/EvmIntrinsics.cpp](../solidity-llvm/src/codegen/EvmIntrinsics.cpp)

```cpp
EvmIntrinsics declareEvmIntrinsics(llvm::Module& m) {
    auto* i256 = IntegerType::get(ctx, 256);
    auto* voidTy = Type::getVoidTy(ctx);

    // i256 @evm.sload(i256 slot)
    auto* sloadTy  = FunctionType::get(i256, {i256}, false);
    e.sload = Function::Create(sloadTy, ExternalLinkage, "evm.sload", &m);

    // void @evm.sstore(i256 slot, i256 value)
    auto* sstoreTy = FunctionType::get(voidTy, {i256, i256}, false);
    e.sstore = Function::Create(sstoreTy, ExternalLinkage, "evm.sstore", &m);
}
```

Это две **функции без тела** (declarations, не definitions). LLVM позволяет
объявить функцию, которая будет «реализована потом» — а в нашем случае она
никогда не реализуется, потому что **наша цель — анализ**, не запуск. Для
анализаторов на дипломе вызовы этих функций — маркеры «здесь обращение к
хранилищу контракта».

### 6.2. `CodeGen::generate` — внешний цикл

[CodeGen.cpp:241-264](../solidity-llvm/src/codegen/CodeGen.cpp#L241-L264)

```cpp
std::unique_ptr<llvm::Module> CodeGen::generate(const ContractDecl& c) {
    ctx_     = make_unique<LLVMContext>();
    module_  = make_unique<Module>(c.name, *ctx_);
    builder_ = make_unique<IRBuilder<>>(*ctx_);
    intr_    = declareEvmIntrinsics(*module_);

    // запомнить storageSlot и тип каждой state-переменной
    for (sv : c.stateVars) {
        stateVarSlots_[sv.name] = sv.storageSlot;
        stateVarTypes_[sv.name] = sv.type.kind;
    }

    // для каждой функции: сначала прототип (forward-объявление), затем тело
    for (fn : c.functions) {
        auto* func = emitFunctionProto(fn);
        emitFunctionBody(fn, func);
    }

    if (llvm::verifyModule(*module_, nullptr)) {
        error("internal: module failed verification");
        return nullptr;
    }
    return std::move(module_);
}
```

**Что такое `LLVMContext`** — глобальное состояние LLVM (типы, константы,
интернированные значения). Один Context = одна потоковая зона. У нас один.

**Что такое `Module`** — контейнер для всего IR одного `.ll` файла. Содержит
функции + глобалы + intrinsic-объявления.

**Что такое `IRBuilder`** — главный «карандаш» LLVM. Вместо ручного
конструирования инструкций пишешь `builder.CreateAdd(L, R, "name")` — он
правильно вставит инструкцию в текущий BasicBlock и проставит SSA-имена.

**`verifyModule`** — встроенный валидатор LLVM. Если он сказал «ок», то IR
гарантированно проходит `llvm-as`. Это наш страховочный пояс — если codegen
накосячит, мы узнаем до того, как пользователь увидит мусор.

### 6.3. Маппинг типов

[CodeGen.cpp:15-22](../solidity-llvm/src/codegen/CodeGen.cpp#L15-L22)

```cpp
llvm::Type* CodeGen::llvmType(TypeKind k) {
    switch (k) {
        case Uint256: return IntegerType::get(*ctx_, 256);   // i256
        case Bool:    return IntegerType::get(*ctx_, 1);     // i1
        case Address: return IntegerType::get(*ctx_, 160);   // i160
    }
}
```

LLVM умеет integer **произвольной ширины** — это и есть ключевая возможность,
из-за которой мы и выбрали LLVM. `i256` нативного аналога на x86 нет, но для
анализа это не требуется.

### 6.4. Прототип функции

[CodeGen.cpp:24-35](../solidity-llvm/src/codegen/CodeGen.cpp#L24-L35)

```cpp
auto* ft = FunctionType::get(retTy, paramTys, /*isVarArg=*/false);
return Function::Create(ft, ExternalLinkage, fn.name, module_.get());
```

`ExternalLinkage` — функция видна снаружи модуля (как extern в C). Это не
важно для нашего анализа, но это default-выбор и LLVM от него не возражает.

### 6.5. Тело функции и параметры через alloca

[CodeGen.cpp:37-65](../solidity-llvm/src/codegen/CodeGen.cpp#L37-L65)

```cpp
void CodeGen::emitFunctionBody(const FunctionDecl& fn, Function* func) {
    locals_.clear();
    auto* entry = BasicBlock::Create(*ctx_, "entry", func);
    builder_->SetInsertPoint(entry);

    // alloca + store для каждого параметра
    auto argIt = func->arg_begin();
    for (param : fn.params) {
        auto* ty  = llvmType(param.type.kind);
        auto* alc = builder_->CreateAlloca(ty, nullptr, param.name);
        builder_->CreateStore(&*argIt, alc);
        locals_[param.name] = LocalSlot{alc, ty};
        ++argIt;
    }

    for (stmt : fn.body.stmts) emitStmt(*stmt);

    // safety net: если последний блок не терминирован — добавить ret
    if (!builder_->GetInsertBlock()->getTerminator()) {
        if (func->getReturnType()->isVoidTy()) builder_->CreateRetVoid();
        else builder_->CreateRet(Constant::getNullValue(func->getReturnType()));
    }
}
```

**Главный приём — alloca для всего.** Параметры в LLVM приходят как SSA-значения
(`%0`, `%1`, …), но работать с ними напрямую неудобно: SSA = каждой
переменной одно присваивание. Поэтому для **каждого** параметра выделяем
ячейку в стеке (`alloca`) и копируем туда значение (`store`). Дальше всё —
через `load`/`store`.

То же самое для локалок (см. `emitStmt(VarDecl)` ниже). Это **в точности тот
же стиль, как делает clang** для C/C++. Преимущество: проще писать codegen,
никаких phi-нод вручную. LLVM-проход `mem2reg` потом за один проход
конвертирует это всё в чистый SSA с phi-нодами.

> «Что такое SSA?» — Static Single Assignment, форма IR, где каждая
> переменная присваивается ровно один раз. Это резко упрощает dataflow-анализ.
> Phi-ноды — это «слияния» значений из разных branch’ей.

> «Что такое phi-нода?» — Инструкция вида `%x = phi [%a, %thenBB], [%b, %elseBB]`,
> которая выбирает `%a` или `%b` в зависимости от того, из какого блока мы
> пришли. У меня их нет — потому что я использую `alloca` и `mem2reg`
> сгенерил бы их, если бы я запустил оптимизатор.

### 6.6. `emitExpr` — выражения

[CodeGen.cpp:87-154](../solidity-llvm/src/codegen/CodeGen.cpp#L87-L154)

| AST-узел | LLVM-вывод |
|---|---|
| `NumberLit "5"` | `i256 5` (`APInt(256, "5", 10)`) |
| `BoolLit true`  | `i1 1` |
| `IdentExpr` (Param/Local) | `load <ty>, ptr %alc` |
| `IdentExpr` (StateVar) | `call i256 @evm.sload(i256 <slot>)` (+ возможный `trunc`) |
| `BinaryExpr Add` | `add` |
| `BinaryExpr Sub` | `sub` |
| `BinaryExpr Mul` | `mul` |
| `BinaryExpr Div` | `udiv` (**unsigned** — uint256 беззнаковый) |
| `BinaryExpr Eq`  | `icmp eq` |
| `BinaryExpr NotEq` | `icmp ne` |
| `BinaryExpr Lt/Gt/LtEq/GtEq` | `icmp ult/ugt/ule/uge` (**беззнаковые**) |
| `BinaryExpr AndAnd` | `and` (**без short-circuit**) |
| `BinaryExpr OrOr`   | `or` (**без short-circuit**) |
| `UnaryExpr Not`     | `xor v, 1` (для `i1`: `0 ^ 1 = 1`, `1 ^ 1 = 0`) |

**Важные комментарии:**

> «Почему `udiv`, а не `sdiv`?» — `uint256` беззнаковый, поэтому деление
> беззнаковое. То же для сравнений: `ult` (unsigned less than), не `slt`.

> «Почему `&&`/`||` без short-circuit?» — В нашем MVP операнды не имеют
> побочных эффектов (нет вызовов функций как expression). Поэтому
> short-circuit не меняет наблюдаемое поведение, и можно эмитить bitwise
> `and`/`or`. Это **корректно, но не классически**. На дипломе, когда добавим
> вызовы функций, придётся переделать на ветвления через BasicBlock’и.

> «Почему `xor v, 1`?» — В LLVM нет отдельной инструкции «not». Не bitwise.
> Идиома — XOR с 1 (для `i1`) или с `-1` (для шире). Для bool достаточно `^1`:
> `0^1=1`, `1^1=0` — это и есть отрицание.

**StateVar чтение** ([CodeGen.cpp:67-75](../solidity-llvm/src/codegen/CodeGen.cpp#L67-L75)):
```cpp
llvm::Value* CodeGen::emitStateLoad(int slot, TypeKind k) {
    auto* slotConst = ConstantInt::get(i256, slot);
    auto* raw = builder_->CreateCall(intr_.sload, {slotConst}, "sload");
    if (targetTy->isIntegerTy(256)) return raw;
    // bool (i1) или address (i160) — нужен trunc
    return builder_->CreateTrunc(raw, targetTy, "trunc");
}
```

EVM-storage всегда хранит 256 бит. Если переменная — `bool` (i1) или
`address` (i160), нам надо обрезать значение через `trunc`. Это копирует
семантику Solidity: «storage — это i256, а тип переменной — view сверху».

**StateVar запись** ([CodeGen.cpp:77-85](../solidity-llvm/src/codegen/CodeGen.cpp#L77-L85)):
```cpp
void CodeGen::emitStateStore(int slot, Value* v, TypeKind /*k*/) {
    Value* wide = v;
    if (!v->getType()->isIntegerTy(256)) {
        wide = builder_->CreateZExt(v, i256, "zext");   // zero-extend
    }
    builder_->CreateCall(intr_.sstore, {slotConst, wide});
}
```

Обратное: если пишем `bool` или `address`, расширяем (`zext`) до i256. Zero-
extend — это «слева добивает нулями» (для беззнаковых типов корректно).

### 6.7. `emitStmt` — statement’ы

[CodeGen.cpp:156-239](../solidity-llvm/src/codegen/CodeGen.cpp#L156-L239)

**Guard в начале:**
```cpp
if (builder_->GetInsertBlock()->getTerminator()) return;
```
Это защита от багов: если предыдущий statement (например `return` в `then`-
ветке) уже терминировал текущий блок, то добавлять дальнейшие инструкции —
**незаконно** по правилам LLVM. Просто пропускаем.

**`VarDecl`**:
```cpp
auto* val = emitExpr(*v.init);
auto* alc = builder_->CreateAlloca(ty, nullptr, v.name);
builder_->CreateStore(val, alc);
locals_[v.name] = LocalSlot{alc, ty};
```
Один к одному с тем, что делал бы clang для `int x = 5;`.

**`Assign`**:
```cpp
auto* val = emitExpr(*a.value);
if (locals_.contains(a.target)) {
    builder_->CreateStore(val, locals_[a.target].alloca);   // локалка
} else {
    emitStateStore(stateVarSlots_[a.target], val, stateVarTypes_[a.target]);
}
```
Сначала ищем target в локалках, не нашли — это state-var, эмитим `sstore`.

**`If`** — самое сложное:
```cpp
auto* cond = emitExpr(*ifs.cond);

auto* thenBB  = BasicBlock::Create(*ctx_, "if.then",  currentFn_);
auto* elseBB  = BasicBlock::Create(*ctx_, "if.else",  currentFn_);
auto* mergeBB = BasicBlock::Create(*ctx_, "if.merge", currentFn_);

builder_->CreateCondBr(cond, thenBB, elseBB);   // условный переход

builder_->SetInsertPoint(thenBB);
for (stmt : thenBlock) emitStmt(*stmt);
if (!terminator) builder_->CreateBr(mergeBB);   // безусловный переход к merge

builder_->SetInsertPoint(elseBB);
for (stmt : elseBlock) emitStmt(*stmt);
if (!terminator) builder_->CreateBr(mergeBB);

if (mergeBB->hasNPredecessors(0)) {
    mergeBB->eraseFromParent();    // обе ветки уже вышли через return
} else {
    builder_->SetInsertPoint(mergeBB);
}
```

Создаём **три блока**:
- `if.then` — тело then
- `if.else` — тело else (даже если else пустой — нужен для условного перехода)
- `if.merge` — куда сходятся обе ветки после if

`CreateCondBr(cond, thenBB, elseBB)` — это и есть `br i1 %cond, label %then, label %else`.
В конце каждой ветки безусловный `br label %merge` (если ветка ещё не
терминирована, например `return`’ом внутри). Дальнейшие statement’ы пишутся в
`mergeBB`.

**Хитрый случай**: обе ветки заканчиваются `return`. Тогда в `mergeBB` никто не
приходит, и он становится мёртвым блоком. LLVM-verify ругается на блоки без
предшественников и terminator’а. Поэтому мы его удаляем (`eraseFromParent`).

**`Return`**:
```cpp
if (r.value) {
    auto* v = emitExpr(*r.value);
    builder_->CreateRet(v);
} else {
    builder_->CreateRetVoid();
}
```

### 6.8. `Require` и `msg.sender`/`msg.value` в codegen

#### `RequireStmt`

[CodeGen.cpp, emitStmt(Require)](../solidity-llvm/src/codegen/CodeGen.cpp)

```cpp
auto* cond = emitExpr(*r.cond);

auto* okBB   = BasicBlock::Create(*ctx_, "require.ok",   currentFn_);
auto* failBB = BasicBlock::Create(*ctx_, "require.fail", currentFn_);

builder_->CreateCondBr(cond, okBB, failBB);

builder_->SetInsertPoint(failBB);
builder_->CreateCall(intr_.revert, {});
builder_->CreateUnreachable();          // ← terminator после noreturn

builder_->SetInsertPoint(okBB);          // ← дальнейшие stmt’ы пишем сюда
```

В IR это выглядит так:
```llvm
br i1 %eq, label %require.ok, label %require.fail

require.ok:
  ; ... продолжение тела функции ...

require.fail:
  call void @evm.revert()
  unreachable
```

**Почему `unreachable`?** LLVM требует, чтобы каждый BasicBlock заканчивался
**terminator’ом** (`ret`, `br`, `unreachable`, `switch`, `invoke`).
`@evm.revert` помечен `noreturn`, но LLVM всё равно нуждается в явном terminator’е
— `unreachable` и есть «control сюда не дойдёт». Это и подсказка
оптимизатору/анализаторам.

#### `msg.sender` / `msg.value`

[CodeGen.cpp, emitExpr(Member)](../solidity-llvm/src/codegen/CodeGen.cpp)

```cpp
const auto& m = static_cast<const MemberExpr&>(e);
if (m.base->kind == ExprKind::Ident) {
    const auto& base = static_cast<const IdentExpr&>(*m.base);
    if (base.name == "msg") {
        if (m.member == "sender")
            return builder_->CreateCall(intr_.msg_sender, {}, "msg.sender");
        if (m.member == "value")
            return builder_->CreateCall(intr_.msg_value, {}, "msg.value");
    }
}
```

**Важно:** мы НЕ зовём `emitExpr(*m.base)`. Если бы позвали, codegen попытался
бы найти `msg` в `locals_` или `stateVarSlots_` — и провалился бы. `msg` это
**синтаксический маркер**, не Symbol. Так же делает Sema.

#### Что осталось НЕ сделано (для диплома, не для MVP)

- **`!` на не-bool** — codegen использует `xor v, 1`. Для `i1` правильно,
  для `i256` неправильно (потерял бы старшие биты). Сейчас семантика гарантирует,
  что операнд `!` — `bool`, так что баг ненаблюдаем.
- **Definite-return analysis** — Sema не проверяет, что **все пути** функции
  делают return. Safety-net в `emitFunctionBody` добивает `ret zero`. По стандарту
  Solidity 0.8 это error, у нас — undefined.
- **Циклы, mapping, modifier, наследование, events, внешние вызовы** — диплом.

---

## 7. Driver и CLI

### 7.1. `Driver::run`

[src/driver/Driver.cpp](../solidity-llvm/src/driver/Driver.cpp)

Сшивает все стадии:

```cpp
1. читаем файл в std::string
2. Lexer + Parser → ContractDecl (или ошибки → stderr → return 1)
3. SemanticAnalyzer → проставляет аннотации (или ошибки → return 1)
4. CodeGen → llvm::Module (или ошибки → return 1)
5. mod->print(raw_string_ostream) → текстовый IR
6. вывести в stdout или в файл
```

LLVM использует **свои собственные** потоки (`llvm::raw_ostream`), поэтому
сначала печатаем в `raw_string_ostream`, потом передаём строку в `std::ostream`.
Это типичный паттерн «мостика».

### 7.2. `main.cpp` (CLI)

[src/main.cpp](../solidity-llvm/src/main.cpp)

Используется библиотека `cxxopts` (header-only из FetchContent). 4 опции:
- `-i,--input <path>` — обязательная
- `-o,--output <path>` — необязательная, по умолчанию stdout
- `--emit-ir` — флаг, чтобы реально запустить компиляцию
- `-h,--help` — печать help

Парсинг обёрнут в try/catch (`cxxopts::exceptions::exception`). Если нет input
— печатается help и `exit(1)`. Если есть `--help` — help и `exit(0)`.

---

## 8. Полный пример: что происходит со строкой `uint256 newValue = value + delta;`

Проследим её **через весь конвейер** в контексте функции `increment(uint256 delta)`:

### Лексер
```
[KwUint256] [Identifier "newValue"] [Eq] [Identifier "value"] [Plus] [Identifier "delta"] [Semicolon]
```

### Парсер
1. `parseStmt()` видит `KwUint256` → ветка `VarDecl`
2. `parseType()` → `Type{Uint256}`
3. читает имя `newValue`
4. ожидает `Eq` → ok
5. `parseExpr()` → `parseBinary(1)`:
   - `parseUnary()` → `parsePrimary()` → `IdentExpr("value")`
   - смотрит `+`, prec=5 ≥ 1
   - потребляет `+`, рекурсивно `parseBinary(6)`:
     - `parseUnary()` → `parsePrimary()` → `IdentExpr("delta")`
     - смотрит `;`, prec=0, выходит
   - возвращает `BinaryExpr(Add, value, delta)`
6. ожидает `;` → ok
7. возвращает `VarDeclStmt(Uint256, "newValue", BinaryExpr(...))`

### Sema
1. `checkStmt(VarDecl)`:
   - `checkExpr(BinaryExpr Add)`:
     - `checkExpr(IdentExpr "value")`: `lookup("value")` → state-var, slot=0, type=uint256.
       Заполняет `resolvedKind=StateVar`, `resolvedSlot=0`, `resolvedType=uint256`.
     - `checkExpr(IdentExpr "delta")`: `lookup("delta")` → param, type=uint256.
       Заполняет `resolvedKind=Param`, `resolvedType=uint256`.
     - Оба uint256, оператор `+` → `resolvedType = uint256`. OK.
   - тип инициализатора (uint256) совпадает с декларируемым (uint256). OK.
   - объявляет локалку `newValue` в текущем скоупе.

### CodeGen
1. `emitStmt(VarDecl)`:
   - `emitExpr(BinaryExpr Add)`:
     - `emitExpr(IdentExpr "value")` (StateVar, slot=0) →
       `%sload = call i256 @evm.sload(i256 0)`
     - `emitExpr(IdentExpr "delta")` (Param) →
       `%delta1 = load i256, ptr %delta`
     - `CreateAdd` → `%add = add i256 %sload, %delta1`
   - `CreateAlloca(i256, "newValue")` → `%newValue = alloca i256`
   - `CreateStore(%add, %newValue)` → `store i256 %add, ptr %newValue`
   - сохранить в `locals_["newValue"] = ...`

Итоговый кусок IR:
```llvm
%sload = call i256 @evm.sload(i256 0)
%delta1 = load i256, ptr %delta, align 4
%add = add i256 %sload, %delta1
%newValue = alloca i256, align 8
store i256 %add, ptr %newValue, align 4
```

И это **ровно то, что выдаёт компилятор** на демо-фикстуре `counter_no_require.sol`.

---

## 9. Сводная таблица «вопрос → ответ»

| Вопрос комиссии | Краткий ответ |
|---|---|
| Сколько кода написал? | ~1870 строк C++, 7 подсистем, 93 теста (GoogleTest). |
| Архитектура? | 4 этапа: лексер → парсер → семантика → codegen. Между ними AST. |
| Лексер — генератор или ручной? | Ручной FSM на ~160 строк. |
| Парсер? | Рекурсивный спуск + precedence climbing (Pratt) для выражений. LL(1). |
| Lookahead? | Один токен (`current_` в парсере, `peek(N)` в лексере). |
| Error recovery? | Synchronize-to-semicolon и synchronize-to-brace-end. |
| Как устроены имена? | Стек скоупов (`SymbolTable`), `unordered_map<string, Symbol>` в каждом. |
| Сколько типов? | 3: `uint256` → `i256`, `bool` → `i1`, `address` → `i160`. |
| Почему `i256`? | LLVM умеет integer любой ширины. Для анализа не нужен реальный execute. |
| Где `msg.sender`? | И семантика, и codegen распознают `msg` синтаксически (не через Symbol). Codegen эмитит `call i160 @evm.msg_sender()`. |
| Как хранятся state-vars? | По storage slots 0, 1, 2 — назначаются в `Sema::registerContractMembers`. |
| Что такое EVM intrinsic? | `declare`-функция без тела: `@evm.sload`, `@evm.sstore`. Маркер для будущих анализаторов. |
| `&&`/`||` со short-circuit? | Нет в MVP — bitwise `and`/`or`. Корректно, потому что в подмножестве нет выражений с побочными эффектами. |
| Что валидирует IR? | `llvm::verifyModule` внутри codegen + `llvm-as` снаружи. |
| Сколько тестов? | 93, GoogleTest, юнит-тесты на каждую подсистему + intergration на `counter.sol`. |
| Как собирается? | CMake + Ninja, LLVM 18 через `find_package`. Альтернативно — Docker (multi-stage). |
| Что **не сделано**? | Циклы, mapping, наследование, модификаторы, события, внешние вызовы, definite-return analysis. Всё это — диплом. |
| Научная новизна? | Перевод Solidity в LLVM IR с EVM intrinsics, на котором можно строить LLVM-passes для детекции уязвимостей (reentrancy, tx.origin и др.) — это диплом. |

---

## 10. Чек-лист «знаю — не знаю»

Если ты можешь по каждому пункту хотя бы 2-3 предложения сказать — ты ответишь
на любой вопрос комиссии.

- [ ] Что такое лексер. Что такое токен. Что такое lookahead.
- [ ] Что такое AST и зачем дерево, а не плоский список.
- [ ] Что такое рекурсивный спуск. Что такое LL(1).
- [ ] Precedence climbing — как `1 + 2 * 3` парсится в правильный приоритет.
- [ ] Что такое symbol table и зачем стек скоупов.
- [ ] Зачем нужна семантика, если парсер уже что-то проверил.
- [ ] Что такое LLVM IR. Что такое module, function, basic block.
- [ ] Что такое SSA. Почему мы вместо SSA пишем alloca/load/store.
- [ ] Что такое intrinsic в нашем контексте (псевдо-функция-маркер).
- [ ] Что такое storage в EVM. Как номера слотов попадают в IR.
- [ ] Как работают `&&`/`||` в нашем codegen и почему без short-circuit.
- [ ] Что такое `i256`/`i160`/`i1`, почему LLVM так умеет.
- [ ] Что валидирует `llvm-as`, что валидирует `verifyModule`.

Если по чему-то «не знаю» — открывай соответствующий раздел этого файла и
читай ещё раз. И отвечай на свой вопрос сам, пока не получится сделать это
без подсматривания.
