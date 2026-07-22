# История разработки `solidity-llvm`

Журнал работы **только по коду компилятора**. Новые записи — сверху (обратный хронологический порядок).

---

## 2026-05-28 — Закрытие MVP: require + msg.sender/msg.value

Демо-контракт `Counter` из CLAUDE.md теперь компилируется end-to-end (раньше работал только `counter_no_require.sol`).

### Изменения
- `EvmIntrinsics`: добавлены три объявления — `void @evm.revert()` (с `setDoesNotReturn`), `i160 @evm.msg_sender()`, `i256 @evm.msg_value()`.
- `CodeGen::emitStmt(Require)`: эмитит `br i1 cond, %require.ok, %require.fail`. В `fail`-блоке — `call @evm.revert + unreachable`. `unreachable` обязателен как terminator после noreturn-вызова и как маркер мёртвого пути для будущих анализаторов. После `require` builder остаётся в `ok`-блоке — последующие statement'ы пишутся туда.
- `CodeGen::emitExpr(Member)`: синтаксическое распознавание `msg.sender`/`msg.value`. `m.base` не эмитим через `emitExpr` — `msg` не настоящий Symbol, в Sema он тоже распознаётся по имени без lookup.

### Тесты (+4)
- `CodeGen.DeclaresRevertAndMsgIntrinsics` — проверка наличия и сигнатур трёх новых intrinsic.
- `CodeGen.RequireEmitsRevertBranch` — в IR ровно 1 вызов evm.revert + 1 unreachable + ≥1 условный br.
- `CodeGen.MsgSenderAndValueCallIntrinsics` — два теста подряд, msg.sender/msg.value эмитятся как вызовы интринзиков.
- `CodeGen.CounterFullVerifies` — настоящий `tests/fixtures/counter.sol` (с require, msg.sender) → `verifyModule == false` (нет ошибок).

Итого 93 → 97 тестов, все зелёные.

### Верификация
- `./build/src/solidity-llvm --input tests/fixtures/counter.sol --emit-ir --output /tmp/counter.ll && llvm-as /tmp/counter.ll` — проходит без ошибок.
- В IR видно: `%msg.sender = call i160 @evm.msg_sender()`, `br i1 %eq, label %require.ok, label %require.fail`, `require.fail: call void @evm.revert(); unreachable`, attribute `noreturn` на evm.revert.

### Зачем
Формальный критерий MVP из CLAUDE.md: «Counter из ТЗ компилируется в `.ll`, проходящий `llvm-as`» — теперь закрыт.

---

## 2026-04-18 — Неделя 1: скелет проекта

Репозиторий создан: `~/diplom/solidity-llvm/`, ветка `main`.

### Файловая структура на конец недели
```
solidity-llvm/
├── CMakeLists.txt
├── README.md
├── .gitignore
├── cmake/FetchDeps.cmake
├── include/solc/driver/Driver.h
├── src/
│   ├── CMakeLists.txt
│   ├── main.cpp
│   └── driver/Driver.cpp
└── tests/
    ├── CMakeLists.txt
    └── test_smoke.cpp
```

### Ключевые решения

- **LLVM версия в CMake:** `find_package(LLVM 18.1 REQUIRED CONFIG)`. Вариант `18` не матчится против установленной `18.1.3` — LLVMConfigVersion.cmake требует хотя бы minor.
- **Зависимости:** cxxopts v3.2.0 и GoogleTest v1.14.0 через FetchContent. vcpkg не используется.
- **Архитектура:**
  - статическая либа `solc_core` (будет расти по неделям) → экспортирует `solc::Driver`
  - executable `solidity-llvm` → только обёртка над CLI (cxxopts) + вызов Driver
  - PUBLIC линковка LLVM include/defs/libs у `solc_core`, чтобы тесты и будущие компоненты их наследовали
- **LLVM компоненты:** `core support irreader bitwriter analysis` (минимум, который понадобится в последующих неделях).
- **CMake слои:** `add_subdirectory(src)` и `add_subdirectory(tests)` + `enable_testing()`. `CMAKE_EXPORT_COMPILE_COMMANDS ON` для clangd.

### Реализованные компоненты

**`Driver` (stub)** — `include/solc/driver/Driver.h`, `src/driver/Driver.cpp`:
- `struct CompileOptions { std::string inputPath; std::string outputPath; bool emitIR = false; }`
- `class Driver { int run(const CompileOptions&); }`
- Поведение: открыть input → если не открылся, `error: cannot open '<path>'` в stderr + return 1. Иначе прочитать всё в string. При `emitIR=true` — вывести `; solidity-llvm: stub output, source length=<N>` в stdout или в `outputPath`-файл. Return 0.
- Реальной кодогенерации нет — Driver будет допиливаться каждую неделю.

**CLI (`src/main.cpp`)** — cxxopts:
- `-i/--input` (required), `-o/--output` (default `""`), `--emit-ir`, `-h/--help`
- Нет `--input` или есть `--help` → печать help. `--help` → exit 0; отсутствие input → exit 1.
- `opts.parse` пока без try/catch (отложено на week 8).

**Smoke-тесты (`tests/test_smoke.cpp`)** — GoogleTest через `gtest_discover_tests`:
- `Smoke.DriverRejectsMissingFile` — `/no/such/file.sol` → ненулевой exit
- `Smoke.DriverAcceptsExistingFile` — /tmp-файл с `contract X {}` + emitIR → exit 0

### Коммиты
```
4a3300c docs: readme with build & run instructions
abe0ae2 test: smoke tests for Driver via GoogleTest
e92972b feat: stub CLI and Driver linked against LLVM
ff54990 build: CMake skeleton with LLVM 18.1 and FetchContent deps
2165c4e chore: init repo with gitignore
```

Все коммиты перезаписаны на автора `gdumfi@yandex.ru` через `git filter-branch --env-filter`.

### Верификация
- `ninja` — clean, без warnings
- `ctest --output-on-failure` — 2/2 Passed
- `./build/src/solidity-llvm --help` — печатает все опции
- `build/compile_commands.json` генерируется

### Технический долг
Не блокирует, плановые чистки в week 8:
- `include/solc/driver/Driver.h` — неиспользуемый `#include <string_view>`
- `src/main.cpp` — `opts.parse` без try/catch
- `src/driver/Driver.cpp` — нет проверки `file.is_open()` для output-файла

### Что дальше
Неделя 2 — лексер. Все 35 `TokenKind` подмножества MVP, ручной однопроходный лексер со `std::string_view`, lookahead 1, трекинг `SourceLoc{line, col}`, скип `//` и `/* */`, диагностики через `DiagnosticEngine`. Фикстура `lex_counter.sol` должна лексироваться без ошибок.

---

## 2026-04-18 — Неделя 2: лексер

Всего 8 тестов Lexer + 1 тест Diag = 9 новых unit-тестов. Итого в проекте 11 тестов, все зелёные.

### Новые компоненты

**`DiagnosticEngine` (`include/solc/diag/Diagnostic.h`, `src/diag/Diagnostic.cpp`)**
- `struct SourceLoc { uint32_t line=1, col=1; }` — позиция в исходнике (1-based).
- `struct Diagnostic { SourceLoc loc; std::string message; }`.
- `class DiagnosticEngine`: методы `error(SourceLoc, string)`, `errors()`, `hasErrors()`. Плоский вектор ошибок, по мере роста модулей будет принимать диагностики от lexer → parser → sema.

**`Token` и `TokenKind` (`include/solc/lexer/Token.h`)**
- 38 значений `TokenKind` разбиты на группы: литералы/идентификаторы 4 (Identifier, Number, True, False), типы 3 (KwUint256, KwBool, KwAddress), ключевые слова 8 (contract, function, public, returns, return, if, else, require), операторы 14 (+ - * / == != < > <= >= && || ! =), пунктуация 7 (( ) { } , ; .), служебные 2 (EndOfFile, Invalid).
- `struct Token { TokenKind kind; std::string_view lexeme; SourceLoc loc; }` — zero-copy ссылка на исходник.
- `const char* tokenKindName(TokenKind)` — для диагностики/отладки.

**`Lexer` (`include/solc/lexer/Lexer.h`, `src/lexer/Lexer.cpp`)**
- Конструктор берёт `std::string_view source` + `DiagnosticEngine&` по ссылке — не владеет ни источником, ни диагностикой.
- Однопроходный, lookahead 1 (`peek(offset=0)`).
- Трекинг `loc_{line, col}`: `advance()` инкрементит col, на `\n` сбрасывает col и инкрементит line.
- `skipWhitespaceAndComments()` — пробельные, `// line-comment`, `/* block-comment */` (без вложенности — Solidity не поддерживает).
- `next()` — диспатч по первому символу:
  1. letter/`_` → `lexIdentifierOrKeyword` (consumes alnum/underscore, linear lookup в `constexpr` таблице из 13 ключевых слов)
  2. digit → `lexNumber` (только десятичные цифры, без hex/underscore/scientific — MVP)
  3. switch по знакам пунктуации и двухсимвольным операторам через `match(c)`
  4. всё остальное → diagnostic `unexpected char '...'` + Invalid-токен

### Фикстуры и тесты

`tests/CMakeLists.txt`: через `target_compile_definitions(unit_tests PRIVATE SOLC_TESTS_DIR="...")` тесты получают абсолютный путь до `tests/` на этапе компиляции → фикстуры читаются независимо от рабочей директории `ctest`.

`tests/fixtures/`:
- `counter.sol` — полный демо-контракт из CLAUDE.md (value, owner, increment с require/msg.sender, isOwner).
- `lex_comments.sol` — минимальный файл со строчным и блочным комментарием.

`tests/test_lexer.cpp` — 9 TEST:
- `Diag.CollectsErrors`
- `Lexer.SingleCharPunct`, `MultiCharOps`, `KeywordsAndIdents`, `Numbers`, `NumberLexemePreserved`
- `Lexer.CounterFixtureNoDiagnostics`, `CommentsFixtureLexesCleanly`
- `Lexer.UnexpectedCharProducesDiag`

### Коммиты (8 новых, итого 13)
```
b297ed5 test(lexer): invalid char produces diagnostic
dc6faeb test(lexer): Counter.sol and comments fixtures lex without diagnostics
038e5b9 feat(lexer): decimal number literals
3e5c83f feat(lexer): identifiers and keywords
3bffd99 feat(lexer): multi-char operators
55fedef feat(lexer): single-char punctuation + whitespace/comments
c4f67e2 feat(lexer): Token, TokenKind and tokenKindName()
20684ad feat(diag): DiagnosticEngine with SourceLoc
```

### Архитектурные решения

- **Лексер не владеет буфером.** `std::string_view` — caller (Driver) отвечает за время жизни исходника. Никаких лишних аллокаций.
- **Ошибки — не исключения.** При ошибке лексер пишет диагностику в `DiagnosticEngine` и возвращает `Invalid`-токен с корректной позицией. Парсер на неделе 3-4 сможет решать, recover'иться или прерываться.
- **SourceLoc захватывается ДО `advance()`.** Иначе `loc` токена указывал бы на конец, а не начало лексемы.
- **Ключевые слова как `static constexpr std::pair<string_view, TokenKind>`-массив с линейным поиском.** 13 записей — линейный поиск дешевле хеш-таблицы; никаких аллокаций при старте.

### Технический долг (накопительно)

- *(из недели 1)* `include/solc/driver/Driver.h` — неиспользуемый `#include <string_view>`.
- *(из недели 1)* `src/main.cpp` — `opts.parse` без try/catch.
- *(из недели 1)* `src/driver/Driver.cpp` — нет проверки `file.is_open()` для output-файла.
- **Новое:** unterminated `/* ... */` тихо добегает до EOF без диагностики. Добавить `diag_.error` при `peek()=='\0'` перед выходом из внутреннего цикла.
- **Новое:** числа с ведущими нулями (`007`) проходят без ошибки. Solidity их запрещает. Добавить валидацию или отдельный диагностический pass.
- **Новое:** сообщение «unexpected char '&'» на одиночном `&` — можно улучшить до «expected '&&'». Косметика.

### Что дальше
Неделя 3 — парсер объявлений. `contract X { state-vars; function-headers }`. Тело функций пока будет пропускаться (учёт `{` и `}`), реальный разбор statements — на неделе 4.

---

## 2026-04-18 — Неделя 3: парсер объявлений

17 новых тестов parser/AST, итого 28/28 зелёные. Полный `counter.sol` теперь парсится в AST без диагностик. Тело функций пока пропускается (учёт `{`/`}`).

### Новые компоненты

**AST узлы декларативного уровня (`include/solc/ast/AST.h`, `src/ast/AST.cpp`)** — namespace `solc::ast`:
- `enum class TypeKind { Uint256, Bool, Address }`
- `struct Type { TypeKind kind; SourceLoc loc; }`
- `struct Param { Type type; std::string name; SourceLoc loc; }`
- `struct StateVarDecl { Type type; std::string name; bool isPublic; SourceLoc loc; }`
- `struct Block { SourceLoc loc; }` — заглушка до недели 4
- `struct FunctionDecl { std::string name; std::vector<Param> params; std::optional<Type> returnType; Block body; SourceLoc loc; }`
- `struct ContractDecl { std::string name; std::vector<StateVarDecl> stateVars; std::vector<FunctionDecl> functions; SourceLoc loc; }`
- Свободные функции `void dump(const ContractDecl&, std::ostream&)`, `const char* typeKindName(TypeKind)`

**`Parser` (`include/solc/parser/Parser.h`, `src/parser/Parser.cpp`)**:
- Ctor `(Lexer&, DiagnosticEngine&)` — оба по ссылке, ничего не владеет.
- 1-токен lookahead через `Token current_`, обновляется `advance()`.
- Public API (методы возвращают `std::optional<...>`; для unit-тестируемости публичны и промежуточные продукции):
  - `parseContract()` — top-level
  - `parseFunctionDecl()`
  - `parseStateVarDecl()`
  - `parseType()`
- Private token-machinery: `peek/check/match/advance/expect`.
- Private helpers: `parseParam()`, `skipBraceBody()` (counter депт `{`/`}` до парного `}`), `synchronizeToSemicolon()`, `synchronizeToBraceEnd()`.
- Грамматика (реализованная):
  ```
  ContractDecl    := 'contract' Ident '{' ContractMember* '}'
  ContractMember  := StateVarDecl | FunctionDecl
  StateVarDecl    := Type 'public'? Ident ';'
  FunctionDecl    := 'function' Ident '(' ParamList? ')' 'public'
                     ('returns' '(' Type ')')? '{' /* skipped */ '}'
  Param           := Type Ident
  Type            := 'uint256' | 'bool' | 'address'
  ```

**Error recovery (Task 8)**:
- `synchronizeToSemicolon` — продвигается до `;` (включительно), `}`, или EOF.
- `synchronizeToBraceEnd` — продвигается до `{` (вызывает `skipBraceBody`), `}`, или EOF.
- Все ошибочные пути `parseStateVarDecl` начинают с `synchronizeToSemicolon()`; пути `parseFunctionDecl` (кроме первого `expect(KwFunction)`) — с `synchronizeToBraceEnd()`.
- `parseContract` на nullopt от члена контракта НЕ возвращает nullopt — продолжает цикл и собирает остальное.

### Тесты (17 новых)

`tests/test_parser.cpp`:
- `AST.DumpEmptyContract`
- `Parser.EmptyInputProducesError`
- `Parser.ParseTypeUint256/Bool/Address/UnknownFails`
- `Parser.ParseStateVarPublic/NoPublic/MissingName`
- `Parser.FunctionNoParamsNoReturn/OneParamReturnsUint/TwoParams/BodyWithNestedBracesSkipped`
- `Parser.FullContractWithVarsAndFunctions/ContractEmptyBody`
- `Parser.CounterFixtureFullDeclarations` — полный `counter.sol`: 2 state vars, 2 функции с правильными типами/именами параметров
- `Parser.RecoversAfterBadStateVar` — `uint256 ; address owner;` → 1 ошибка + state var `owner` собран

### Коммиты (6 новых, итого 21)
```
c8878a3 feat(parser): error recovery at ';' and '}'
9184341 test(parser): Counter.sol parses without diagnostics
800a6f2 feat(parser): top-level contract declaration
53b8efd feat(parser): function declaration with skipped body
1fc2a0c feat(parser): state variable declaration
36b6ec9 feat(parser): parseType for uint256/bool/address
786a8b7 feat(parser): skeleton with token machinery
273d10c feat(ast): declarations AST + dump
```

### Архитектурные решения

- **AST — POD-структуры, не классы.** Никаких конструкторов/виртуальных методов. Владение через `std::string`, `std::vector`, `std::optional`. Это упрощает отладку (`dump` — свободная функция) и сериализацию в будущем.
- **`parseType`, `parseStateVarDecl`, `parseFunctionDecl` публичны.** Позволяет unit-тестировать продукции независимо. На неделе 8 можно скрыть или оставить — не критично.
- **Тело функции пропускается через `skipBraceBody` с подсчётом глубины `{`/`}`.** Корректно для произвольного содержимого, в т.ч. вложенных `if {}` и `else {}`. Реальный разбор — неделя 4.
- **Error recovery — минимальный.** Точки синхронизации: `;` (statements/decls), `{`/`}` (function body). Достаточно для MVP. Полноценный recovery с follow-set'ами — на потом.
- **Каждый AST-узел несёт `SourceLoc`.** Захватывается ДО `advance()`. Будет нужно для диагностик sema (неделя 5).

### Технический долг (накопительно)

- *(неделя 1)* Driver.h: неиспользуемый `<string_view>`; main.cpp: `opts.parse` без try/catch; Driver.cpp: нет `file.is_open()` для output.
- *(неделя 2)* Unterminated `/* */` без диагностики; числа с ведущими нулями не флагуются; одиночные `&`/`|` — generic "unexpected char".
- **Новое (неделя 3):** `skipBraceBody` молча возвращается на EOF без диагностики «unterminated body». `RecoversAfterBadStateVar` не проверяет текст диагностики (только `hasErrors()`). Нет negative-теста на сломанную функцию (`RecoversAfterBadFunction`).

### Что дальше
Неделя 4 — парсер statements и expressions. Реальный разбор тела функций: `if/else`, `return`, `require(...)`, объявление и присваивание локальных, арифметика с precedence climbing. По итогам `Block` перестанет быть заглушкой, AST `Counter.sol` будет полным.

---

## 2026-04-18 — Технический долг (недели 1-3)

7 фиксов накопленного техдолга, 7 коммитов, 35/35 тестов зелёные. Без новой функциональности — только UX и диагностики.

### Что починили

**Неделя 1 (Driver/CLI):**
- `1356877 refactor(driver): remove unused <string_view> include` — почистили `Driver.h`.
- `4648993 fix(cli): catch cxxopts parse errors and show help` — `opts.parse` теперь в try/catch (`cxxopts::exceptions::exception&`). На битых аргументах: stderr с сообщением + help + exit 1. Никаких uncaught exceptions.
- `d744d56 fix(driver): fail when output file cannot be opened` — после `file.open(opts.outputPath)` проверка `is_open()`. На неудаче: `error: cannot open output '...'` + return 1. Новый тест `Smoke.DriverFailsOnUnwritableOutput` (выводит в `/no/such/dir/...`).

**Неделя 2 (лексер):**
- `321ffcf fix(lexer): diagnose unterminated /* */ comments` — `skipWhitespaceAndComments` захватывает `commentStart` при входе в `/*`, при достижении EOF без `*/` пишет диагностику `unterminated block comment`. Тест `Lexer.UnterminatedBlockCommentProducesDiag`.
- `25caa31 fix(lexer): diagnose number literals with leading zeros` — `lexNumber` после потребления цифр проверяет `lex.size() > 1 && lex[0] == '0'` и пишет `number literal with leading zero is not allowed` (соответствие Solidity 0.5+). Токен `Number` всё равно возвращается, лексема `"007"` сохраняется — диагностика отдельна от формы токена. Тесты: `LeadingZeroProducesDiag`, `SingleZeroIsFine` (одиночный `0` — ок), `ZeroFollowedBySpaceIsFine`.

**Неделя 3 (парсер):**
- `a5a517d fix(parser): diagnose unterminated '{' body` — `skipBraceBody` захватывает `openLoc` при входе, при EOF без парного `}` пишет `unterminated '{' body`. Тест `Parser.UnterminatedFunctionBodyProducesDiag`.
- `aa6d4a6 test(parser): tighten state-var recovery + add bad-function recovery test` — `RecoversAfterBadStateVar` дополнен проверкой подстроки `"identifier"` в сообщении. Новый тест `RecoversAfterBadFunction`: `function () public { return 1; } uint256 public ok;` — recovery через `synchronizeToBraceEnd` поглощает тело и подбирает `ok`. Зашёл с первой попытки — recovery в `parseFunctionDecl` уже было правильно настроено, не хватало только теста.

### Изменения в тестовом покрытии
- Было 28 тестов после недели 3 → стало 35.
- +1 smoke (Driver fail on output), +4 lexer (unterminated comment, leading zeros, single zero, zero-space), +2 parser (unterminated body, bad-function recovery).

### Что НЕ делали и почему
- Single `&` / `|` → выдают generic `unexpected char '&'`. Косметика; «expected `&&`» было бы приятнее, но `&&`/`||` без второго символа в Solidity не имеют смысла, поэтому текущий вывод адекватен.
- Не реализовывали интеграционный тест на crash-free CLI с битыми аргументами — спланировано на неделю 8 (полировка CLI).

### Накопленный техдолг (после уборки)
Пусто. Все известные пункты разобраны.

### Что дальше
Возвращаемся к плану: неделя 4 — парсер statements и expressions. Реальный разбор тел функций, precedence climbing для выражений.

---

## 2026-04-18 — Неделя 4: парсер statements и expressions

Тело функций больше не пропускается — реальный разбор. AST `counter.sol` теперь **полный**, с раскрытыми телами обеих функций. 23 новых теста, итого 58/58 зелёные.

### Новые AST-узлы

**Expressions** (`include/solc/ast/AST.h`):
- `enum class ExprKind { NumberLit, BoolLit, Ident, Binary, Unary, Member, Call }`
- Базовый `struct Expr { ExprKind kind; SourceLoc loc; virtual ~Expr()=default; }` + `using ExprPtr = std::unique_ptr<Expr>;`
- Узлы (наследники Expr): `NumberLit{text}`, `BoolLit{value}`, `IdentExpr{name}`, `BinaryExpr{op, lhs, rhs}`, `UnaryExpr{op, operand}`, `MemberExpr{base, member}`, `CallExpr{callee, args}`
- `enum class BinOp { Add, Sub, Mul, Div, Eq, NotEq, Lt, Gt, LtEq, GtEq, AndAnd, OrOr }`
- `enum class UnOp { Not }`
- Helpers `binOpName(BinOp)`, `unOpName(UnOp)`

**Statements**:
- `enum class StmtKind { VarDecl, Assign, If, Return, Require }`
- Базовый `struct Stmt` + `using StmtPtr = std::unique_ptr<Stmt>;`
- `VarDeclStmt{type, name, init}`, `AssignStmt{target, value}`, `IfStmt{cond, thenBlock, elseBlock}`, `ReturnStmt{value}`, `RequireStmt{cond}`

**Block** обновлён: `std::vector<StmtPtr> stmts;`. Move-only — каскадно `FunctionDecl` тоже move-only; `Parser.cpp` использует `fn.body = std::move(body)`.

### Новые методы `Parser`

**Public**:
- `parseExpr()` — entry-point для выражений
- `parseStmt()` — entry-point для statements
- (Существующий `parseFunctionDecl` теперь зовёт `parseBlockStatements`, а не `skipBraceBody`)

**Private**:
- `parsePrimary()` — литералы, ident, `(expr)`, постфикс-цикл для `.member` и `(args)` (chained: `a.b.c`, `f()(x)`)
- `parseUnary()` — рекурсивно для `!`, иначе `parsePrimary`
- `parseBinary(int minPrec)` — precedence climbing
- `parseBlockStatements(vector<StmtPtr>&)` — `{ stmts* }` с error recovery через `synchronizeToSemicolon`
- `static binOpPrecedence(TokenKind) -> int` — таблица приоритетов 1..6
- `static tokenToBinOp(TokenKind) -> optional<BinOp>`

### Precedence table

| Уровень | Операторы | Ассоциативность |
|---|---|---|
| 1 | `\|\|` | левая |
| 2 | `&&` | левая |
| 3 | `==`, `!=` | левая |
| 4 | `<`, `>`, `<=`, `>=` | левая |
| 5 | `+`, `-` | левая |
| 6 | `*`, `/` | левая |
| 7 unary | `!` | префикс |
| 8 postfix | `.member`, `(args)` | левая |
| 9 primary | literal/ident/`(expr)` | — |

Левая ассоциативность реализована через `parseBinary(prec + 1)` для правого операнда.

### Грамматика недели 4

```
Stmt          := IfStmt | ReturnStmt | RequireStmt | VarDeclStmt | AssignStmt
IfStmt        := 'if' '(' Expr ')' Block ('else' Block)?
ReturnStmt    := 'return' Expr ';'
RequireStmt   := 'require' '(' Expr ')' ';'
VarDeclStmt   := Type Ident '=' Expr ';'
AssignStmt    := Ident '=' Expr ';'
Block         := '{' Stmt* '}'
Expr          := parseBinary(1)
parseBinary   := unary (binOp parseBinary(prec+1))*
parseUnary    := '!' parseUnary | parsePrimary
parsePrimary  := Number | True | False | '(' Expr ')'
                 | Ident postfix*
postfix       := '.' Ident | '(' (Expr (',' Expr)*)? ')'
```

### Архитектурные решения

- **Discriminated union через class hierarchy** — `Expr`/`Stmt` имеют `Kind` + `virtual ~`, наследники добавляют поля. Downcast через `static_cast<T*>(e.get())` после проверки `e->kind == ExprKind::T`. Альтернатива (`std::variant`) сложнее с рекурсивными типами, отказались.
- **Precedence climbing**, а не Pratt parser. Меньше кода, легче читать, идентичный результат.
- **Cross-cut при интеграции:** диагностика `"unterminated '{' body"` перенесена из `skipBraceBody` в `parseBlockStatements`, чтобы сохранить покрытие `Parser.UnterminatedFunctionBodyProducesDiag`. `skipBraceBody` остался для error-recovery в `synchronizeToBraceEnd` — там диагностика не нужна.
- **`parseBlockStatements` используется в 3 местах:** тело функции, then-блок if, else-блок if. Единый код, единое поведение.

### Тесты (23 новых)

`tests/test_parser.cpp`:
- AST: `ExprConstructionAndKinds`, `StmtConstructionAndKinds`, `BlockHoldsStatements`
- Primary: `ParsePrimaryNumberLit/BoolLit/Ident/Parenthesized`, `ParseMemberAccess`, `ParseCallNoArgs/TwoArgs`
- Unary/Binary: `ParseUnaryNot`, `ParseAddMulPrecedence`, `ParseEqAndAssociativity`, `ParseUnaryOverBinary`, `LeftAssociativity`
- Stmt: `StmtReturnExpr`, `StmtRequire`, `StmtVarDecl`, `StmtAssign`, `IfStmtNoElse`, `IfStmtWithElse`
- Integration: `FunctionBodyParsed`, `CounterFixtureFullBodies`

### Коммиты (8 новых, итого 38)
```
05726fb test(parser): Counter.sol bodies parsed end-to-end
7cfd4ea feat(parser): function body parsed as block of statements
8ccc85f feat(parser): if/else statement and parseBlockStatements
7298e84 feat(parser): statements — return/require/var-decl/assign
e83b235 feat(parser): unary ! and binary operators with precedence climbing
aa70d1c feat(parser): parsePrimary — literals, idents, member, call, parens
5144ae5 feat(ast): statement nodes + Block with stmt vector
13ed730 feat(ast): expression nodes (Number/Bool/Ident/Binary/Unary/Member/Call)
```

### Что теперь умеет компилятор
Полный `counter.sol` парсится в AST с раскрытыми телами:
- `increment` — 4 stmts: Require, VarDecl, Assign, Return
- `isOwner` — 1 stmt: Return

`require(msg.sender == owner)` → `RequireStmt{cond=BinaryExpr{Eq, MemberExpr{IdentExpr"msg", "sender"}, IdentExpr"owner"}}`.
Каждая `+`, каждое сравнение, каждый member access — узел в дереве.

### Технический долг
- `dump()` не печатает тела функций (только `{ ... }`). Расширить можно на неделе 8 при добавлении `--ast-dump`.
- `else if (...) { ... }` без явного else-блока внутри else-ветки — не поддержано (в плане это вне MVP). Workaround: `} else { if (...) { ... } }`.
- В `parseStmt` ident-led branch ловит любое начало с идентификатора как assignment — не expression statement. В MVP это ок, но при добавлении expression statements (диплом) — нужно lookahead на следующий токен.

### Что дальше
Неделя 5 — семантический анализ. Symbol table со scope stack, проверка типов на каждом Expr, привязка IdentExpr к Symbol, проверка `msg.sender`/`msg.value` как builtins, проверка `require` как builtin.

---

## 2026-05-13 — Неделя 5: семантический анализ

22 новых теста, итого 80/80 зелёные. `counter.sol` end-to-end через lexer → parser → sema без единой диагностики. AST обогащён аннотациями типов и резолвинга.

### Новые компоненты

**Symbol model (`include/solc/sema/Symbol.h`, `src/sema/Symbol.cpp`)** — namespace `solc::sema`:
- `enum class SymbolKind { StateVar, Param, Local, Function }`
- `struct Symbol { kind; name; ast::Type type; int storageSlot; SourceLoc declLoc; }`
- `class Scope { add(Symbol); find(name); }` — обёртка над `unordered_map<string, Symbol>`
- `class SymbolTable` — `vector<Scope> stack_` с операциями `enterScope/leaveScope/declare/lookup/lookupCurrentScope`

**`SemanticAnalyzer` (`include/solc/sema/Sema.h`, `src/sema/Sema.cpp`)**:
- Ctor `(DiagnosticEngine&)`. Public `bool analyze(ContractDecl&)`.
- Private: `registerContractMembers`, `analyzeFunction`, `checkStmt`, `checkExpr`.
- Поле `const ast::Type* currentReturnType_ = nullptr` — для проверки `return` против сигнатуры функции.

### AST annotations (по значению, без указателей)

Plan-reviewer обнаружил критический баг: `Symbol*` указатели в AST становятся dangling после `leaveScope()`. Codegen на неделе 6 это бы сломал. Решение — хранить аннотации по значению:

В `include/solc/ast/AST.h`:
- `struct Expr`: добавлено `std::optional<Type> resolvedType` — заполняется любым успешным `checkExpr`.
- `struct IdentExpr`: добавлен `enum class Resolution { Unresolved, StateVar, Param, Local, Function }`, поля `Resolution resolvedKind = Unresolved`, `int resolvedSlot = -1` (storageSlot для StateVar).
- `struct StateVarDecl`: добавлено `int storageSlot = -1` — заполняется в `registerContractMembers`.

`Parser.cpp` пришлось доправить: в aggregate initialization `StateVarDecl{ty, name, isPublic, ...}` добавлено `-1` для нового поля. Subagent сам заметил и починил.

### Правила типизации

| Expr | Правило | resolvedType |
|---|---|---|
| `NumberLit` | — | `Uint256` |
| `BoolLit` | — | `Bool` |
| `IdentExpr` | resolvedSymbol exists в scope | type символа |
| `MemberExpr(IdentExpr("msg"), "sender")` | builtin | `Address` |
| `MemberExpr(IdentExpr("msg"), "value")` | builtin | `Uint256` |
| `BinaryExpr +-*/` | оба `Uint256` | `Uint256` |
| `BinaryExpr ==!=` | sameType(lhs, rhs) | `Bool` |
| `BinaryExpr <><=>=` | оба `Uint256` | `Bool` |
| `BinaryExpr &&\|\|` | оба `Bool` | `Bool` |
| `UnaryExpr !` | операнд `Bool` | `Bool` |
| `CallExpr` | — | reject (require — это statement) |

### Stmt-правила

- `VarDecl`: checkExpr(init), type kind равен declared, declare локальную.
- `Assign`: lookup target, checkExpr(value), type kind value == type target.
- `If`: cond → Bool, рекурсивно scoped checkStmt по thenBlock/elseBlock.
- `Return`: с currentReturnType_ — value обязателен и того же типа; без него — value обязан отсутствовать.
- `Require`: cond → Bool.

### Scope hierarchy

```
analyze:
  global scope (state vars + functions)
  ↓
  для каждой function:
    function scope (params)
    ↓
    iterate body.stmts (locals declared here)
    ↓
    в IfStmt:
      then-scope
      else-scope (отдельно)
```

`leaveScope()` всегда вызывается — даже на ошибочных путях. Internal Symbol* указатели не утекают, а в AST уходят только копии данных (kind + slot + type).

### Тесты (22 новых)

`tests/test_sema.cpp`:
- SymbolTable: DeclareAndLookup, DuplicateInSameScopeFails, ShadowingInInnerScope
- Sema regs: StateVarsGetSlotsAndRegisterFunctions, DuplicateStateVarFails, DuplicateFunctionFails, DuplicateParamFails
- Primary: UndeclaredIdentFails, LiteralTypes
- Operators: BinaryArithRequiresUint, EqAcceptsSameTypeReturnsBool, UnaryNotRequiresBool
- Builtins: MsgSenderIsAddress, MsgValueIsUint256, MsgUnknownMemberFails
- Require: RequireAcceptsBool, RequireRejectsNonBool
- Mismatch: VarDeclTypeMismatch, ReturnTypeMismatch, AssignTypeMismatch, IfCondMustBeBool
- Integration: CounterFixturePassesSema (storageSlot 0/1)

### Коммиты (9 новых, итого 45)
```
43a0508 test(sema): Counter.sol passes full sema with no diagnostics
4248207 feat(sema): statement type-checks (if/return/var-decl/assign)
c70b634 feat(sema): require statement bool check
dc761bf feat(sema): msg.sender and msg.value builtins
1f272b1 feat(sema): binary and unary operator typing
3816d50 feat(sema): type-check literals and identifier resolution
8db1349 feat(sema): function parameter scope
35a9130 feat(sema): contract-level symbols + state var storage slots
8809308 feat(sema): Symbol, Scope, SymbolTable
```

### Архитектурные решения

- **AST annotations by value, не by pointer.** Plan-reviewer пред-обнаружил dangling-pointer риск. Цена: дублирование данных (size of Resolution + slot). Выигрыш: AST self-contained после sema, codegen не зависит от живости SymbolTable.
- **`msg.sender`/`msg.value` — спец-кейс в `checkExpr(Member)`.** НЕ кладём `msg` в SymbolTable как настоящий Symbol — обрабатываем напрямую через распознавание `IdentExpr("msg")` + member name. Иначе lookup на `msg` упирался бы в "undeclared".
- **`CallExpr` всегда reject.** В MVP единственный builtin — `require`, обрабатывается как `RequireStmt` (так разбирает парсер). Любой `f()` как expression → diagnostic.
- **`currentReturnType_` — поле SemanticAnalyzer, не параметр checkStmt.** Сейчас нет вложенных функций, но архитектурно поле сейв/ресторится в `analyzeFunction` — готово к расширению.

### Технический долг (накопительно)

- *(неделя 4)* `dump()` не печатает тела функций.
- *(новое)* Sema не интегрирована в Driver — запускается только в тестах. Будет на неделе 8 при шлифовке CLI.
- *(новое)* `Eq/NotEq` принимает любые равные типы — корректно для MVP, но без комментария-обоснования.
- *(новое)* Default-ветки в `checkStmt`/`checkExpr` оставлены как safety net; теоретически недостижимы после Tasks 5-8.

### Что дальше
Неделя 6 — codegen. LLVM IRBuilder, маппинг типов (`uint256` → `i256`, `bool` → `i1`, `address` → `i160`), `alloca` для local vars, `evm.sload`/`evm.sstore` intrinsics для state vars, арифметика, `if/else` через `br`/`phi`, `return`. Первый валидный `.ll` для Counter (без `require`/`msg.*` — это неделя 7).

---

## 2026-05-13 — Неделя 6: codegen (LLVM IR)

Первый настоящий артефакт компилятора. `counter_no_require.sol` end-to-end через CLI → валидный `.ll` файл, проходящий `llvm-as`. 13 новых тестов, 93/93 зелёные.

### Новые компоненты

**`EvmIntrinsics` (`include/solc/codegen/EvmIntrinsics.h`, `src/codegen/EvmIntrinsics.cpp`)**:
- `struct EvmIntrinsics { llvm::Function* sload; llvm::Function* sstore; }`
- `declareEvmIntrinsics(Module&)` — объявляет два intrinsic'а с `ExternalLinkage`:
  - `declare i256 @evm.sload(i256 slot)`
  - `declare void @evm.sstore(i256 slot, i256 value)`
- Остальные intrinsic'и (`evm.caller`, `evm.callvalue`, `evm.revert`) — на неделе 7.

**`CodeGen` (`include/solc/codegen/CodeGen.h`, `src/codegen/CodeGen.cpp`)**:
- Owned: `unique_ptr<LLVMContext>`, `unique_ptr<Module>`, `unique_ptr<IRBuilder<>>`, `EvmIntrinsics intr_`.
- Per-function state: `unordered_map<string, LocalSlot> locals_` (alloca + LLVM type для opaque-pointer-compatible load), `Function* currentFn_`.
- Per-contract maps: `stateVarSlots_`, `stateVarTypes_` (нужны для AssignStmt — у AST AssignStmt::target нет resolved kind/slot).
- Public API: `generate(const ContractDecl&) → unique_ptr<Module>`. Возвращает nullptr если `verifyModule` упал.

### Маппинг типов и storage

| Solidity | LLVM | Notes |
|---|---|---|
| `uint256` | `i256` | LLVM умеет 256-битные целые |
| `bool` | `i1` | один бит |
| `address` | `i160` | 20 байт EVM-адреса |

**Storage I/O:** все операции через intrinsic'и:
- Чтение: `%v = call i256 @evm.sload(i256 N)`, затем `trunc` если нужен `i1`/`i160`.
- Запись: `zext` до `i256` если нужно, затем `call void @evm.sstore(i256 N, i256 %v)`.

**Локальные/параметры:** через `alloca` в entry-блоке + `load`/`store`. Параметры тоже копируются в alloca при входе в функцию — стандартный clang-style паттерн (`-O0`).

### Реализованный обход AST

**`emitExpr`** — switch по `ExprKind`:
- `NumberLit` → `ConstantInt::get(ctx, APInt(256, text, 10))` (работает с любыми большими литералами из строки)
- `BoolLit` → i1 const
- `IdentExpr`:
  - Param/Local → `load type, alloca`
  - StateVar → `emitStateLoad(slot, kind)`
- `BinaryExpr` — switch по BinOp:
  - Add/Sub/Mul/Div → CreateAdd/Sub/Mul/**UDiv** (беззнаковый — uint256)
  - Lt/Gt/LtEq/GtEq → CreateICmp**U**LT/UGT/ULE/UGE (беззнаковые)
  - Eq/NotEq → ICmpEQ/NE (signedness-neutral)
  - AndAnd/OrOr → bitwise And/Or (MVP — без short-circuit, операнды чистые)
- `UnaryExpr(Not)` → `CreateXor(V, 1)` (для i1 = логическое отрицание)
- `MemberExpr`, `CallExpr` — рейзят diag (msg.*/require на неделе 7)

**`emitStmt`** — switch по `StmtKind`, с **guard'ом в начале** `if (builder_->GetInsertBlock()->getTerminator()) return;`:
- `VarDecl` → alloca + store + register в `locals_`
- `Assign` → emitExpr, если target в locals_ → store, иначе → emitStateStore
- `If` → 3 BB (then/else/merge), CondBr, каждая ветка терминируется (либо явный return, либо br merge), если merge мёртв — `eraseFromParent`
- `Return` → CreateRet или CreateRetVoid
- `Require` — на неделе 7 (default: return)

### Driver integration

`Driver::run` теперь на `--emit-ir`:
1. Открывает входной файл (как раньше)
2. Lexer → Parser → SemanticAnalyzer → CodeGen
3. Любая ошибка на этапе — диагностики в stderr с строкой/колонкой, return 1
4. На успехе — `module->print(raw_string_ostream)` в stdout или в `--output` файл

Stub-эмиссия `; solidity-llvm: stub output...` удалена.

### Тесты (13 новых)

`tests/test_codegen.cpp`:
- Skeleton: `EmitsEmptyModule`, `DeclaresSloadAndSstore`
- Prototypes: `FunctionPrototypesEmitted`, `ParameterAllocaInEntry`
- Literals/Ident: `ReturnsParamThroughLoad`
- State I/O: `StateVarReadEmitsSload`, `StateVarWriteEmitsSstore`
- Operators: `AddProducesIRAdd`, `EqEmitsICmpEQ`, `NotEmitsXorWithOne`
- VarDecl: `VarDeclCreatesAllocaAndStore`
- Control flow: `IfElseEmitsBranchAndBlocks`
- Integration: `CounterNoRequireVerifies` (полная фикстура counter_no_require.sol)

Существующие `Smoke.DriverAcceptsExistingFile` (с пустым контрактом) продолжают работать через новый pipeline.

### Коммиты (9 новых, итого 54)
```
f2735a4 chore(codegen): fix typo in if/else merge-block comment
eef3287 feat(driver): full pipeline lex->parse->sema->codegen on --emit-ir
879c56b feat(codegen): if/else control flow with merge block
a30b155 feat(codegen): variable declaration statement
b1b7542 feat(codegen): binary and unary operators
49ea0c6 feat(codegen): state variable read/write via evm.sload/sstore
d216d01 feat(codegen): literals, identifier loads, return statement
6489822 feat(codegen): function prototypes + entry block + parameter allocas
b87fcc4 feat(codegen): CodeGen skeleton with EVM intrinsics declarations
```

### Архитектурные решения

- **LLVMContext lifetime — главная ловушка.** `Module` хранит non-owning ref на `LLVMContext`, который владеет `CodeGen`. Если функция возвращает `unique_ptr<Module>` напрямую и `CodeGen` уничтожается — Module указывает на dangling context, и любое обращение (включая `verifyModule` в деструкторе) → SEGV. Решение в тестах: helper `compileSrc` возвращает `struct Compiled { unique_ptr<CodeGen>; unique_ptr<Module>; }` — порядок destruction корректен. В Driver — `CodeGen` живёт до конца блока emitIR.
- **Opaque pointers (LLVM 16+).** `CreateLoad` требует явный тип. Поэтому `LocalSlot { alloca, type }` — храним и AllocaInst, и Type.
- **AssignStmt target — без аннотации.** AST хранит только имя, не Resolution. CodeGen строит `stateVarSlots_`/`stateVarTypes_` maps один раз при входе в `generate`. При Assign: lookup в `locals_` (shadowing-aware), иначе в state-var maps.
- **If/else merge block — корректный edge-case.** Если обе ветки `return` — merge никем не используется. Использовать `hasNPredecessors(0) → eraseFromParent` + guard `getTerminator()` в начале `emitStmt`. Plan-reviewer пред-обнаружил эту проблему (без неё verifier бы упал на любой `if/else` с return в обеих ветках).
- **`&&`/`||` без short-circuit.** В MVP операнды — чистые выражения (Sema гарантирует bool). Bitwise `And`/`Or` на i1 даёт правильный результат. Если позже понадобятся side-effects в правом операнде — переделаем через blocks+phi.
- **`UDiv`/`ICmpU*`** — беззнаковые операции для uint256. Знаковые (sdiv, slt) обработали бы high bit как знак.

### Что теперь работает

```bash
cd ~/diplom/solidity-llvm
./build/src/solidity-llvm --input tests/fixtures/counter_no_require.sol --emit-ir --output /tmp/counter.ll
llvm-as /tmp/counter.ll -o /tmp/counter.bc      # exit 0
```

Вывод (фрагмент):
```llvm
declare i256 @evm.sload(i256)
declare void @evm.sstore(i256, i256)

define i256 @increment(i256 %0) {
entry:
  %delta = alloca i256, align 8
  store i256 %0, ptr %delta, align 4
  %sload = call i256 @evm.sload(i256 0)
  %delta1 = load i256, ptr %delta, align 4
  %add = add i256 %sload, %delta1
  ...
}
```

Это **настоящий LLVM IR**. С ним работают `opt`, `llc`, и в будущем — наши собственные passes для детекции уязвимостей.

### Технический долг

- *(существующий)* `dump()` AST не печатает тела.
- *(новый)* `require`/`msg.*` не поддержаны — counter.sol (с require) сейчас провалится на codegen (Member и Call в emitExpr выдадут diag). Это неделя 7.
- *(новый)* Safety net в `emitFunctionBody` (CreateRetVoid если нет терминатора) — корректна, но потенциально маскирует баги если codegen-визитер забудет терминировать функцию.
- *(новый)* `&&`/`||` без short-circuit — задокументировано, корректно для MVP.

### Что дальше
Неделя 7 — финальный фронтенд. Добавим intrinsic'и `evm.caller`, `evm.callvalue`, `evm.revert`. Реализуем `msg.sender`/`msg.value` как builtin-вызовы в codegen. `require(cond)` → CondBr на ok/revert блоки. По итогам полный `counter.sol` (с `require(msg.sender == owner)`) компилируется в IR.

---

<!-- Новые сессии добавляются выше этой линии -->
