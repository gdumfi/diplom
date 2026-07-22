# Week 3 — Parser: Declarations

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans.

**Goal:** Рекурсивный-спуск парсер, который разбирает структуру контракта: `contract Name { state-vars; function headers с телом-стабом }`. Тело функции на этой неделе парсится как пустое `{}` или skip до закрывающей скобки — statements будут на неделе 4.

**Architecture:** `Parser` — класс, владеющий `Lexer` (через ссылку на `DiagnosticEngine`). Использует 1-токен lookahead (`peek_` кэш). Возвращает `std::unique_ptr<ContractDecl>` (или `nullptr` при ошибке). AST — plain-structs с enum `NodeKind` или sum type через std::variant.

**Tech Stack:** C++20, `std::unique_ptr`, lexer из week 2.

---

## Files

- Create: `include/solc/ast/AST.h` (на этой неделе — только декларации + заглушки для Stmt/Expr)
- Create: `src/ast/AST.cpp` (dump для отладки)
- Create: `include/solc/parser/Parser.h`
- Create: `src/parser/Parser.cpp`
- Modify: `src/CMakeLists.txt`
- Create: `tests/test_parser.cpp`
- Modify: `tests/CMakeLists.txt`
- Create: `tests/fixtures/parse_counter.sol` (копия Counter, но с пустыми телами если нужно)

---

## AST (минимум для недели 3)

```cpp
namespace solc::ast {

enum class TypeKind { Uint256, Bool, Address };

struct Type {
    TypeKind kind;
    SourceLoc loc;
};

struct Param {
    Type type;
    std::string name;
    SourceLoc loc;
};

struct StateVarDecl {
    Type type;
    std::string name;
    bool isPublic = false;
    SourceLoc loc;
};

// заглушка — на неделе 4 станет std::vector<StmtPtr>
struct Block {
    SourceLoc loc;
    // неделя 3: тело не парсим в детали; храним raw range токенов или пропускаем
};

struct FunctionDecl {
    std::string name;
    std::vector<Param> params;
    std::optional<Type> returnType;
    Block body;
    SourceLoc loc;
};

struct ContractDecl {
    std::string name;
    std::vector<StateVarDecl> stateVars;
    std::vector<FunctionDecl> functions;
    SourceLoc loc;
};

}  // namespace solc::ast
```

---

## Grammar (implemented this week)

```
SourceFile := ContractDecl EOF
ContractDecl := 'contract' Ident '{' ContractMember* '}'
ContractMember := StateVarDecl | FunctionDecl
StateVarDecl := Type 'public'? Ident ';'
FunctionDecl := 'function' Ident '(' ParamList? ')' 'public'
                ('returns' '(' Type ')')? Block
ParamList := Param (',' Param)*
Param := Type Ident
Type := 'uint256' | 'bool' | 'address'
Block := '{' /* skipped until week 4 */ '}'
```

---

## Tasks (сжато, TDD)

### Task 1: AST-заголовки + dump
- [ ] Создать `include/solc/ast/AST.h` с типами выше
- [ ] `src/ast/AST.cpp` — функции `dump(const ContractDecl&, std::ostream&)`
- [ ] Тест `AST.Dump_EmptyContract` — строит вручную `ContractDecl{"X"}`, `dump` выводит `contract X {}`
- [ ] Commit: `feat(ast): declarations AST + dump`

### Task 2: Parser skeleton — `peek()`, `expect()`, `consume()`
- [ ] `Parser.h/cpp` с приватными: `Token peek_`, `advance()`, `check(TokenKind)`, `match(TokenKind)`, `expect(TokenKind, const char* ctx)` (пишет в diag и возвращает false).
- [ ] Тест `Parser.EmptyInputErrors` — пустой src → diagnostic "expected 'contract'".
- [ ] Commit: `feat(parser): skeleton with token machinery`

### Task 3: `parseType`
- [ ] Реализовать: `KwUint256` → `TypeKind::Uint256` и т.д. Иначе diagnostic.
- [ ] Тесты: `Parser.ParsesUint256`, `Parser.ParsesBool`, `Parser.ParsesAddress`, `Parser.UnknownTypeErrors`.
- [ ] Commit: `feat(parser): parseType`

### Task 4: `parseStateVarDecl`
- [ ] Формат: `Type ('public')? Ident ';'`.
- [ ] Тест на `uint256 public value;` → `StateVarDecl{Uint256, "value", isPublic=true}`.
- [ ] Тест на `address owner;` без public.
- [ ] Negative: `uint256 ;` → diagnostic.
- [ ] Commit: `feat(parser): state variable declaration`

### Task 5: `parseFunctionDecl` (с пропуском тела)
- [ ] Формат: `'function' Ident '(' ParamList? ')' 'public' ('returns' '(' Type ')')? '{' ... '}'`.
- [ ] Тело: парсим `{`, считаем вложенные `{`/`}`, пока не закроем. Содержимое выбрасываем.
- [ ] Тесты:
  - `function inc(uint256 d) public returns (uint256) { }` — 1 param, return Uint256.
  - `function z() public { }` — 0 params, без returns.
  - `function multi(uint256 a, address b) public { }` — 2 params.
- [ ] Commit: `feat(parser): function declaration with skipped body`

### Task 6: `parseContract` — объединение
- [ ] `'contract' Ident '{' ContractMember* '}'`.
- [ ] Решение state-var vs function: если `peek == KwFunction` → FunctionDecl, иначе StateVarDecl.
- [ ] Тест: `contract Counter { uint256 public v; address o; function f() public {} }`
  - ContractDecl.name == "Counter"
  - 2 state vars
  - 1 function
- [ ] Commit: `feat(parser): top-level contract declaration`

### Task 7: Интеграция с фикстурой Counter
- [ ] `tests/fixtures/parse_counter.sol` — полный Counter из CLAUDE.md (тела функций парсятся skip).
- [ ] Test `Parser.ParsesCounterFixture`:
  - Нет диагностик
  - 2 state vars: value/Uint256/public=true, owner/Address/public=true
  - 2 функции: increment (1 param), isOwner (1 param)
- [ ] Commit: `test(parser): Counter.sol fixture parses cleanly`

### Task 8: Error recovery (минимум)
- [ ] При ошибке в state-var — прыжок до `;`.
- [ ] При ошибке в function-header — прыжок до `{` и skip тела.
- [ ] Тест: два state-var, один сломан — парсер даёт 1 ошибку и второй var всё равно попадает в AST.
- [ ] Commit: `feat(parser): basic error recovery at `;` and `{``

---

## Критерий готовности недели 3

- [ ] `parse_counter.sol` → `ContractDecl` без диагностик
- [ ] ≥ 10 unit-тестов парсера зелёные
- [ ] `dump(contractDecl)` печатает читаемое представление
- [ ] Есть хоть один негативный тест с recovery
