# Week 5 — Semantic Analysis

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development или superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Семантический анализатор обогащает AST: разрешение имён (state vars, параметры, локальные, builtin `msg`), типизация каждого Expr, диагностика несоответствий. На выходе — тот же AST, но у каждого `IdentExpr` стоит `resolvedSymbol`, у каждого `Expr` — `resolvedType`, у каждой `StateVarDecl` — `storageSlot`.

**Architecture:** Класс `SemanticAnalyzer` обходит AST после успешного парсинга. Внутренний `SymbolTable` со scope-stack (`std::vector<Scope>`): глобальный (state vars + functions) → функция (params) → блок (locals). Ошибки в `DiagnosticEngine`. Builtin `msg` обрабатывается в `MemberExpr` особо (не как настоящий Symbol).

**Tech Stack:** C++20, продолжение недели 4.

---

## File Structure

- **Create:** `include/solc/sema/Symbol.h` — `SymbolKind`, `Symbol`, `Scope`, `SymbolTable`.
- **Create:** `src/sema/Symbol.cpp` — SymbolTable impl.
- **Create:** `include/solc/sema/Sema.h` — `class SemanticAnalyzer`.
- **Create:** `src/sema/Sema.cpp` — реализация анализатора.
- **Modify:** `include/solc/ast/AST.h` — добавить `std::optional<Type> resolvedType` в `Expr`; в `IdentExpr` добавить **аннотации по значению** (без указателей): `enum class Resolution { Unresolved, StateVar, Param, Local, Function }` + `Resolution resolvedKind`, `int resolvedSlot` (для StateVar); `int storageSlot = -1` в `StateVarDecl`. Зачем по значению: указатели в `Symbol*` будут dangling после `leaveScope()`, codegen на неделе 6 это сломает.
- **Modify:** `src/CMakeLists.txt` — добавить sema-источники в `solc_core`.
- **Create:** `tests/test_sema.cpp` — ~13 TEST.
- **Modify:** `tests/CMakeLists.txt` — добавить `test_sema.cpp`.

---

## Symbol model

```cpp
namespace solc::sema {

enum class SymbolKind { StateVar, Param, Local, Function };

struct Symbol {
    SymbolKind     kind;
    std::string    name;
    ast::Type      type;            // для Function — тип возврата (если есть)
    int            storageSlot = -1;// только для StateVar
    SourceLoc      declLoc;
};

class Scope {
public:
    bool          add(Symbol s);        // false если дубликат в этом scope
    Symbol*       find(std::string_view name);   // только в этом scope
private:
    std::unordered_map<std::string, Symbol> table_;
};

class SymbolTable {
public:
    void     enterScope();
    void     leaveScope();
    bool     declare(Symbol s);                 // false если дубликат в текущем scope
    Symbol*  lookup(std::string_view name);     // ищет от вершины стека вниз
    Symbol*  lookupCurrentScope(std::string_view name);
private:
    std::vector<Scope> stack_;
};

}  // namespace solc::sema
```

Builtin `msg` НЕ кладём в SymbolTable — это спец-кейс в visitMemberExpr.

---

## Правила типизации

| Expr | Условие | resolvedType |
|---|---|---|
| `NumberLit` | — | `Uint256` |
| `BoolLit` | — | `Bool` |
| `IdentExpr` | resolvedSymbol существует | `sym.type` |
| `MemberExpr(IdentExpr("msg"), "sender")` | — | `Address` |
| `MemberExpr(IdentExpr("msg"), "value")` | — | `Uint256` |
| `MemberExpr` иначе | — | diag, type = invalid |
| `BinaryExpr(+ - * /)` | оба `Uint256` | `Uint256` |
| `BinaryExpr(== !=)` | типы операндов равны | `Bool` |
| `BinaryExpr(< > <= >=)` | оба `Uint256` | `Bool` |
| `BinaryExpr(&& \|\|)` | оба `Bool` | `Bool` |
| `UnaryExpr(!)` | операнд `Bool` | `Bool` |
| `CallExpr` | callee — `IdentExpr("require")` и 1 arg `Bool` | `Bool` (формально, не используется) |

---

## Тип-сравнение

Тип «равны» если `TypeKind`-ы совпадают. SourceLoc не сравниваем.

Внутренний хелпер `bool sameType(const ast::Type& a, const ast::Type& b) { return a.kind == b.kind; }` где-нибудь в Sema.cpp.

---

## Tasks

### Task 1: Type-utility, Symbol, SymbolTable

**Files:** Symbol.h, Symbol.cpp, test_sema.cpp, src/CMakeLists.txt, tests/CMakeLists.txt.

- [ ] **Step 1.1: Failing test**

Создай `tests/test_sema.cpp` с includes и базовым тестом:
```cpp
#include <gtest/gtest.h>
#include "solc/sema/Symbol.h"

TEST(SymbolTable, DeclareAndLookup) {
    using namespace solc;
    using namespace solc::sema;

    SymbolTable st;
    st.enterScope();
    Symbol s{SymbolKind::StateVar, "x", ast::Type{ast::TypeKind::Uint256, {}}, 0, {}};
    EXPECT_TRUE(st.declare(s));
    Symbol* found = st.lookup("x");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->name, "x");
    EXPECT_EQ(found->kind, SymbolKind::StateVar);
    EXPECT_EQ(found->storageSlot, 0);
}

TEST(SymbolTable, DuplicateInSameScopeFails) {
    using namespace solc::sema;
    SymbolTable st;
    st.enterScope();
    Symbol a{SymbolKind::Local, "y", solc::ast::Type{solc::ast::TypeKind::Bool, {}}, -1, {}};
    Symbol b = a;
    EXPECT_TRUE(st.declare(a));
    EXPECT_FALSE(st.declare(b));
}

TEST(SymbolTable, ShadowingInInnerScope) {
    using namespace solc::sema;
    SymbolTable st;
    st.enterScope();
    st.declare({SymbolKind::StateVar, "z", solc::ast::Type{solc::ast::TypeKind::Uint256, {}}, 0, {}});
    st.enterScope();
    st.declare({SymbolKind::Local, "z", solc::ast::Type{solc::ast::TypeKind::Bool, {}}, -1, {}});
    Symbol* inner = st.lookup("z");
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->kind, solc::sema::SymbolKind::Local);
    st.leaveScope();
    Symbol* outer = st.lookup("z");
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->kind, solc::sema::SymbolKind::StateVar);
}
```

- [ ] **Step 1.2: `include/solc/sema/Symbol.h`**

```cpp
#pragma once
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "solc/ast/AST.h"
#include "solc/diag/Diagnostic.h"

namespace solc::sema {

enum class SymbolKind { StateVar, Param, Local, Function };

struct Symbol {
    SymbolKind   kind;
    std::string  name;
    ast::Type    type;
    int          storageSlot = -1;
    SourceLoc    declLoc;
};

class Scope {
public:
    bool    add(Symbol s);
    Symbol* find(std::string_view name);
private:
    std::unordered_map<std::string, Symbol> table_;
};

class SymbolTable {
public:
    void    enterScope();
    void    leaveScope();
    bool    declare(Symbol s);
    Symbol* lookup(std::string_view name);
    Symbol* lookupCurrentScope(std::string_view name);
private:
    std::vector<Scope> stack_;
};

}  // namespace solc::sema
```

- [ ] **Step 1.3: `src/sema/Symbol.cpp`**

```cpp
#include "solc/sema/Symbol.h"

namespace solc::sema {

bool Scope::add(Symbol s) {
    auto [it, ok] = table_.emplace(s.name, std::move(s));
    return ok;
}

Symbol* Scope::find(std::string_view name) {
    auto it = table_.find(std::string(name));
    return it == table_.end() ? nullptr : &it->second;
}

void SymbolTable::enterScope() { stack_.emplace_back(); }
void SymbolTable::leaveScope() { stack_.pop_back(); }

bool SymbolTable::declare(Symbol s) {
    return stack_.back().add(std::move(s));
}

Symbol* SymbolTable::lookup(std::string_view name) {
    for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
        if (Symbol* s = it->find(name)) return s;
    }
    return nullptr;
}

Symbol* SymbolTable::lookupCurrentScope(std::string_view name) {
    return stack_.back().find(name);
}

}  // namespace solc::sema
```

- [ ] **Step 1.4: Wire into build**

В `src/CMakeLists.txt` добавь `sema/Symbol.cpp` в источники `solc_core` (создай каталог `src/sema/` если нужно).
В `tests/CMakeLists.txt` добавь `test_sema.cpp`.

- [ ] **Step 1.5: Build + test**
```bash
cd /home/akum1/diplom/solidity-llvm/build
cmake ..
ninja unit_tests && ctest --output-on-failure
```
Expected: 61/61 (58 + 3 новых).

- [ ] **Step 1.6: Commit**
```bash
git add include/solc/sema src/sema src/CMakeLists.txt tests/CMakeLists.txt tests/test_sema.cpp
git commit -m "feat(sema): Symbol, Scope, SymbolTable"
```

---

### Task 2: AST annotations + Sema skeleton + register state vars и functions

**Files:** AST.h, Sema.h, Sema.cpp, src/CMakeLists.txt, test_sema.cpp.

- [ ] **Step 2.1: Failing test**

В `tests/test_sema.cpp` добавь:
```cpp
#include "solc/lexer/Lexer.h"
#include "solc/parser/Parser.h"
#include "solc/sema/Sema.h"

TEST(Sema, StateVarsGetSlotsAndRegisterFunctions) {
    solc::DiagnosticEngine d;
    solc::Lexer lex(
        "contract C {"
        "  uint256 a;"
        "  address b;"
        "  function f() public { }"
        "}", d);
    solc::Parser p(lex, d);
    auto c = p.parseContract();
    ASSERT_TRUE(c.has_value());
    ASSERT_FALSE(d.hasErrors());

    solc::sema::SemanticAnalyzer s(d);
    bool ok = s.analyze(*c);
    EXPECT_TRUE(ok);
    EXPECT_FALSE(d.hasErrors());

    EXPECT_EQ(c->stateVars[0].storageSlot, 0);
    EXPECT_EQ(c->stateVars[1].storageSlot, 1);
}

TEST(Sema, DuplicateStateVarFails) {
    solc::DiagnosticEngine d;
    solc::Lexer lex("contract C { uint256 x; bool x; }", d);
    solc::Parser p(lex, d);
    auto c = p.parseContract();
    ASSERT_TRUE(c.has_value());

    solc::sema::SemanticAnalyzer s(d);
    EXPECT_FALSE(s.analyze(*c));
    EXPECT_TRUE(d.hasErrors());
}

TEST(Sema, DuplicateFunctionFails) {
    solc::DiagnosticEngine d;
    solc::Lexer lex(
        "contract C {"
        "  function f() public { }"
        "  function f() public { }"
        "}", d);
    solc::Parser p(lex, d);
    auto c = p.parseContract();
    ASSERT_TRUE(c.has_value());

    solc::sema::SemanticAnalyzer s(d);
    EXPECT_FALSE(s.analyze(*c));
    EXPECT_TRUE(d.hasErrors());
}
```

- [ ] **Step 2.2: AST annotations**

В `include/solc/ast/AST.h`:

1. Добавь в `struct Expr` поле:
```cpp
    std::optional<Type> resolvedType;
```
(`Type` уже определён, `<optional>` уже подключён.)

2. В `struct IdentExpr` добавь enum + два поля (по значению, не указатели):
```cpp
    enum class Resolution { Unresolved, StateVar, Param, Local, Function };
    Resolution resolvedKind = Resolution::Unresolved;
    int        resolvedSlot = -1;   // storageSlot для StateVar; иначе -1
```

3. Добавь в `struct StateVarDecl` поле:
```cpp
    int storageSlot = -1;
```
(после `bool isPublic`).

**Никаких forward-declare для `sema::Symbol`** — AST в нём не нуждается.

- [ ] **Step 2.3: `include/solc/sema/Sema.h`**

```cpp
#pragma once
#include "solc/ast/AST.h"
#include "solc/diag/Diagnostic.h"
#include "solc/sema/Symbol.h"

namespace solc::sema {

class SemanticAnalyzer {
public:
    explicit SemanticAnalyzer(DiagnosticEngine& diag) : diag_(diag) {}

    // вернёт false если были ошибки. AST модифицируется: storageSlot/resolvedSymbol/resolvedType.
    bool analyze(ast::ContractDecl& contract);

private:
    bool registerContractMembers(ast::ContractDecl& c);

    DiagnosticEngine& diag_;
    SymbolTable       symbols_;
};

}  // namespace solc::sema
```

- [ ] **Step 2.4: `src/sema/Sema.cpp` начальная версия**

```cpp
#include "solc/sema/Sema.h"

#include <string>

namespace solc::sema {

bool SemanticAnalyzer::registerContractMembers(ast::ContractDecl& c) {
    bool ok = true;
    int slot = 0;
    for (auto& sv : c.stateVars) {
        Symbol s{SymbolKind::StateVar, sv.name, sv.type, slot, sv.loc};
        if (!symbols_.declare(std::move(s))) {
            diag_.error(sv.loc, "duplicate state variable '" + sv.name + "'");
            ok = false;
        } else {
            sv.storageSlot = slot;
            slot++;
        }
    }
    for (auto& fn : c.functions) {
        ast::Type ftype = fn.returnType.value_or(ast::Type{ast::TypeKind::Uint256, {}});
        Symbol s{SymbolKind::Function, fn.name, ftype, -1, fn.loc};
        if (!symbols_.declare(std::move(s))) {
            diag_.error(fn.loc, "duplicate function '" + fn.name + "'");
            ok = false;
        }
    }
    return ok;
}

bool SemanticAnalyzer::analyze(ast::ContractDecl& c) {
    symbols_.enterScope();
    bool ok = registerContractMembers(c);
    // тело функций — следующие задачи
    symbols_.leaveScope();
    return ok && !diag_.hasErrors();
}

}  // namespace solc::sema
```

- [ ] **Step 2.5: Wire `sema/Sema.cpp` в `src/CMakeLists.txt`.**

- [ ] **Step 2.6: Build + test**
```bash
cmake .. && ninja unit_tests && ctest --output-on-failure
```
Expected: 64/64.

- [ ] **Step 2.7: Commit**
```bash
git add include/solc/ast/AST.h include/solc/sema/Sema.h src/sema/Sema.cpp src/CMakeLists.txt tests/test_sema.cpp
git commit -m "feat(sema): contract-level symbols + state var storage slots"
```

---

### Task 3: Параметры функций в scope + проверка возврата (без типов тел)

**Files:** Sema.h, Sema.cpp, test_sema.cpp.

- [ ] **Step 3.1: Failing test**
```cpp
TEST(Sema, DuplicateParamFails) {
    solc::DiagnosticEngine d;
    solc::Lexer lex(
        "contract C { function f(uint256 x, uint256 x) public { } }", d);
    solc::Parser p(lex, d);
    auto c = p.parseContract();
    ASSERT_TRUE(c.has_value());
    solc::sema::SemanticAnalyzer s(d);
    EXPECT_FALSE(s.analyze(*c));
    EXPECT_TRUE(d.hasErrors());
}
```

- [ ] **Step 3.2: Расширь `Sema.h` приватным методом:**
```cpp
    bool analyzeFunction(ast::FunctionDecl& fn);
```

- [ ] **Step 3.3: В `Sema.cpp` добавь:**
```cpp
bool SemanticAnalyzer::analyzeFunction(ast::FunctionDecl& fn) {
    symbols_.enterScope();
    bool ok = true;
    for (const auto& p : fn.params) {
        Symbol s{SymbolKind::Param, p.name, p.type, -1, p.loc};
        if (!symbols_.declare(std::move(s))) {
            diag_.error(p.loc, "duplicate parameter '" + p.name + "'");
            ok = false;
        }
    }
    // обход тела — следующие задачи; пока пусто
    symbols_.leaveScope();
    return ok;
}
```

И в `analyze`:
```cpp
bool SemanticAnalyzer::analyze(ast::ContractDecl& c) {
    symbols_.enterScope();
    bool ok = registerContractMembers(c);
    for (auto& fn : c.functions) {
        if (!analyzeFunction(fn)) ok = false;
    }
    symbols_.leaveScope();
    return ok && !diag_.hasErrors();
}
```

- [ ] **Step 3.4: Build + test**
```bash
ninja unit_tests && ctest --output-on-failure
```
Expected: 65/65.

- [ ] **Step 3.5: Commit**
```bash
git commit -am "feat(sema): function parameter scope"
```

---

### Task 4: Type-check Expr — literals и IdentExpr

**Files:** Sema.h, Sema.cpp, test_sema.cpp.

- [ ] **Step 4.1: Failing tests**
```cpp
TEST(Sema, UndeclaredIdentFails) {
    solc::DiagnosticEngine d;
    solc::Lexer lex(
        "contract C { function f() public { uint256 y = unknown; } }", d);
    solc::Parser p(lex, d);
    auto c = p.parseContract();
    ASSERT_TRUE(c.has_value());
    solc::sema::SemanticAnalyzer s(d);
    EXPECT_FALSE(s.analyze(*c));
    ASSERT_TRUE(d.hasErrors());
    bool found = false;
    for (const auto& e : d.errors())
        if (e.message.find("undeclared") != std::string::npos) { found = true; break; }
    EXPECT_TRUE(found);
}

TEST(Sema, LiteralTypes) {
    solc::DiagnosticEngine d;
    solc::Lexer lex(
        "contract C { function f() public { uint256 a = 1; bool b = true; } }", d);
    solc::Parser p(lex, d);
    auto c = p.parseContract();
    ASSERT_TRUE(c.has_value());
    solc::sema::SemanticAnalyzer s(d);
    EXPECT_TRUE(s.analyze(*c));
    EXPECT_FALSE(d.hasErrors());
}
```

- [ ] **Step 4.2: Расширь `Sema.h` приватными:**
```cpp
    bool checkStmt(ast::Stmt& s);
    bool checkExpr(ast::Expr& e);   // выставляет e.resolvedType при успехе
```

- [ ] **Step 4.3: Реализация в `Sema.cpp`**

```cpp
namespace {
bool sameType(const ast::Type& a, const ast::Type& b) {
    return a.kind == b.kind;
}
}

bool SemanticAnalyzer::checkExpr(ast::Expr& e) {
    using namespace ast;
    switch (e.kind) {
        case ExprKind::NumberLit:
            e.resolvedType = Type{TypeKind::Uint256, e.loc};
            return true;
        case ExprKind::BoolLit:
            e.resolvedType = Type{TypeKind::Bool, e.loc};
            return true;
        case ExprKind::Ident: {
            auto& id = static_cast<IdentExpr&>(e);
            Symbol* sym = symbols_.lookup(id.name);
            if (!sym) {
                diag_.error(id.loc, "undeclared identifier '" + id.name + "'");
                return false;
            }
            // копируем нужное в AST по значению — указатели на Symbol небезопасны
            // после leaveScope(), а codegen на неделе 6 это читает.
            switch (sym->kind) {
                case SymbolKind::StateVar: id.resolvedKind = IdentExpr::Resolution::StateVar; break;
                case SymbolKind::Param:    id.resolvedKind = IdentExpr::Resolution::Param;    break;
                case SymbolKind::Local:    id.resolvedKind = IdentExpr::Resolution::Local;    break;
                case SymbolKind::Function: id.resolvedKind = IdentExpr::Resolution::Function; break;
            }
            id.resolvedSlot = sym->storageSlot;
            id.resolvedType = sym->type;
            return true;
        }
        default:
            // остальное в следующих задачах
            diag_.error(e.loc, "expression form not yet supported in sema");
            return false;
    }
}
```

В `analyzeFunction` после declare-параметров добавь обход тела (только VarDecl и Assign пока — простой случай для тестов выше):
```cpp
    for (auto& sp : fn.body.stmts) {
        if (!checkStmt(*sp)) ok = false;
    }
```

И первый набросок `checkStmt`:
```cpp
bool SemanticAnalyzer::checkStmt(ast::Stmt& s) {
    using namespace ast;
    switch (s.kind) {
        case StmtKind::VarDecl: {
            auto& v = static_cast<VarDeclStmt&>(s);
            if (!checkExpr(*v.init)) return false;
            Symbol sym{SymbolKind::Local, v.name, v.type, -1, v.loc};
            if (!symbols_.declare(std::move(sym))) {
                diag_.error(v.loc, "duplicate local '" + v.name + "'");
                return false;
            }
            return true;
        }
        case StmtKind::Assign: {
            auto& a = static_cast<AssignStmt&>(s);
            if (!symbols_.lookup(a.target)) {
                diag_.error(a.loc, "undeclared identifier '" + a.target + "'");
                return false;
            }
            return checkExpr(*a.value);
        }
        default:
            // остальное (If, Return, Require) — следующие задачи
            return true;
    }
}
```

- [ ] **Step 4.4: Build + test**
```bash
ninja unit_tests && ctest --output-on-failure
```
Expected: 67/67. Существующие counter-тесты парсера не должны сломаться — мы трогаем только sema.

- [ ] **Step 4.5: Commit**
```bash
git commit -am "feat(sema): type-check literals and identifier resolution"
```

---

### Task 5: Binary и Unary operators

**Files:** Sema.cpp, test_sema.cpp.

- [ ] **Step 5.1: Failing tests**
```cpp
TEST(Sema, BinaryArithRequiresUint) {
    solc::DiagnosticEngine d;
    solc::Lexer lex(
        "contract C { function f() public { uint256 a = true + 1; } }", d);
    solc::Parser p(lex, d);
    auto c = p.parseContract();
    ASSERT_TRUE(c.has_value());
    solc::sema::SemanticAnalyzer s(d);
    EXPECT_FALSE(s.analyze(*c));
    EXPECT_TRUE(d.hasErrors());
}

TEST(Sema, EqAcceptsSameTypeReturnsBool) {
    solc::DiagnosticEngine d;
    solc::Lexer lex(
        "contract C { function f() public { bool b = 1 == 1; } }", d);
    solc::Parser p(lex, d);
    auto c = p.parseContract();
    ASSERT_TRUE(c.has_value());
    solc::sema::SemanticAnalyzer s(d);
    EXPECT_TRUE(s.analyze(*c));
    EXPECT_FALSE(d.hasErrors());
}

TEST(Sema, UnaryNotRequiresBool) {
    solc::DiagnosticEngine d;
    solc::Lexer lex(
        "contract C { function f() public { bool b = !1; } }", d);
    solc::Parser p(lex, d);
    auto c = p.parseContract();
    ASSERT_TRUE(c.has_value());
    solc::sema::SemanticAnalyzer s(d);
    EXPECT_FALSE(s.analyze(*c));
    EXPECT_TRUE(d.hasErrors());
}
```

- [ ] **Step 5.2: Расширь `checkExpr` (case по `ExprKind`)**

Добавь обработку Binary/Unary. **`default`-ветку оставь** (пишет «expression form not yet supported» + return false) — Member/Call закроются в Tasks 6-7. Иначе между этой задачей и Task 6 функция падает в UB при попадании на Member или Call.

```cpp
        case ExprKind::Binary: {
            auto& bin = static_cast<BinaryExpr&>(e);
            if (!checkExpr(*bin.lhs)) return false;
            if (!checkExpr(*bin.rhs)) return false;
            const Type& lt = *bin.lhs->resolvedType;
            const Type& rt = *bin.rhs->resolvedType;
            switch (bin.op) {
                case BinOp::Add: case BinOp::Sub:
                case BinOp::Mul: case BinOp::Div:
                    if (lt.kind != TypeKind::Uint256 || rt.kind != TypeKind::Uint256) {
                        diag_.error(bin.loc, std::string("operator '") + binOpName(bin.op)
                                                 + "' expects uint256");
                        return false;
                    }
                    e.resolvedType = Type{TypeKind::Uint256, bin.loc};
                    return true;
                case BinOp::Lt: case BinOp::Gt:
                case BinOp::LtEq: case BinOp::GtEq:
                    if (lt.kind != TypeKind::Uint256 || rt.kind != TypeKind::Uint256) {
                        diag_.error(bin.loc, std::string("operator '") + binOpName(bin.op)
                                                 + "' expects uint256");
                        return false;
                    }
                    e.resolvedType = Type{TypeKind::Bool, bin.loc};
                    return true;
                case BinOp::Eq: case BinOp::NotEq:
                    if (!sameType(lt, rt)) {
                        diag_.error(bin.loc, std::string("operator '") + binOpName(bin.op)
                                                 + "' requires operands of the same type");
                        return false;
                    }
                    e.resolvedType = Type{TypeKind::Bool, bin.loc};
                    return true;
                case BinOp::AndAnd: case BinOp::OrOr:
                    if (lt.kind != TypeKind::Bool || rt.kind != TypeKind::Bool) {
                        diag_.error(bin.loc, std::string("operator '") + binOpName(bin.op)
                                                 + "' expects bool");
                        return false;
                    }
                    e.resolvedType = Type{TypeKind::Bool, bin.loc};
                    return true;
            }
            return false;
        }
        case ExprKind::Unary: {
            auto& un = static_cast<UnaryExpr&>(e);
            if (!checkExpr(*un.operand)) return false;
            if (un.op == UnOp::Not) {
                if (un.operand->resolvedType->kind != TypeKind::Bool) {
                    diag_.error(un.loc, "operator '!' expects bool");
                    return false;
                }
                e.resolvedType = Type{TypeKind::Bool, un.loc};
                return true;
            }
            return false;
        }
```

- [ ] **Step 5.3: Build + test**
```bash
ninja unit_tests && ctest --output-on-failure
```
Expected: 70/70.

- [ ] **Step 5.4: Commit**
```bash
git commit -am "feat(sema): binary and unary operator typing"
```

---

### Task 6: msg.sender / msg.value

**Files:** Sema.cpp, test_sema.cpp.

- [ ] **Step 6.1: Failing tests**
```cpp
TEST(Sema, MsgSenderIsAddress) {
    solc::DiagnosticEngine d;
    solc::Lexer lex(
        "contract C { function f() public { address a = msg.sender; } }", d);
    solc::Parser p(lex, d);
    auto c = p.parseContract();
    ASSERT_TRUE(c.has_value());
    solc::sema::SemanticAnalyzer s(d);
    EXPECT_TRUE(s.analyze(*c));
    EXPECT_FALSE(d.hasErrors());
}

TEST(Sema, MsgValueIsUint256) {
    solc::DiagnosticEngine d;
    solc::Lexer lex(
        "contract C { function f() public { uint256 v = msg.value; } }", d);
    solc::Parser p(lex, d);
    auto c = p.parseContract();
    ASSERT_TRUE(c.has_value());
    solc::sema::SemanticAnalyzer s(d);
    EXPECT_TRUE(s.analyze(*c));
    EXPECT_FALSE(d.hasErrors());
}

TEST(Sema, MsgUnknownMemberFails) {
    solc::DiagnosticEngine d;
    solc::Lexer lex(
        "contract C { function f() public { uint256 v = msg.foo; } }", d);
    solc::Parser p(lex, d);
    auto c = p.parseContract();
    ASSERT_TRUE(c.has_value());
    solc::sema::SemanticAnalyzer s(d);
    EXPECT_FALSE(s.analyze(*c));
    EXPECT_TRUE(d.hasErrors());
}
```

- [ ] **Step 6.2: Расширь `checkExpr` веткой Member**

В switch добавь:
```cpp
        case ExprKind::Member: {
            auto& m = static_cast<MemberExpr&>(e);
            // спец-кейс: msg.sender / msg.value (не настоящий Symbol)
            if (m.base->kind == ExprKind::Ident) {
                auto& base = static_cast<IdentExpr&>(*m.base);
                if (base.name == "msg") {
                    if (m.member == "sender") {
                        e.resolvedType = Type{TypeKind::Address, m.loc};
                        return true;
                    }
                    if (m.member == "value") {
                        e.resolvedType = Type{TypeKind::Uint256, m.loc};
                        return true;
                    }
                    diag_.error(m.loc, "unknown member 'msg." + m.member + "'");
                    return false;
                }
            }
            // обычные member-access не поддерживаем в MVP
            diag_.error(m.loc, "member access not supported (only msg.sender/msg.value)");
            return false;
        }
```

- [ ] **Step 6.3: Build + test**
```bash
ninja unit_tests && ctest --output-on-failure
```
Expected: 73/73.

- [ ] **Step 6.4: Commit**
```bash
git commit -am "feat(sema): msg.sender and msg.value builtins"
```

---

### Task 7: require call

**Files:** Sema.cpp, test_sema.cpp.

- [ ] **Step 7.1: Failing tests**
```cpp
TEST(Sema, RequireAcceptsBool) {
    solc::DiagnosticEngine d;
    solc::Lexer lex(
        "contract C { function f() public { require(1 == 1); } }", d);
    solc::Parser p(lex, d);
    auto c = p.parseContract();
    ASSERT_TRUE(c.has_value());
    solc::sema::SemanticAnalyzer s(d);
    EXPECT_TRUE(s.analyze(*c));
    EXPECT_FALSE(d.hasErrors());
}

TEST(Sema, RequireRejectsNonBool) {
    solc::DiagnosticEngine d;
    solc::Lexer lex(
        "contract C { function f() public { require(1); } }", d);
    solc::Parser p(lex, d);
    auto c = p.parseContract();
    ASSERT_TRUE(c.has_value());
    solc::sema::SemanticAnalyzer s(d);
    EXPECT_FALSE(s.analyze(*c));
    EXPECT_TRUE(d.hasErrors());
}
```

Заметь: `require(...)` парсится как `RequireStmt`, не как `CallExpr`. Так что вызовы общего вида (`f()`) на этой неделе мы НЕ обрабатываем — в скоупе MVP только built-in `require` через `RequireStmt`. Если бы захотелось через CallExpr — то была бы отдельная ветка в checkExpr. Пропускаем как «не поддерживается».

- [ ] **Step 7.2: Расширь `checkExpr` веткой Call (отказ для всех)**
```cpp
        case ExprKind::Call: {
            diag_.error(e.loc, "function calls are not supported (require is handled as a statement)");
            return false;
        }
```

Это нужно потому что без default-ветки компилятор выдаст warning о пропущенных кейсах. Если хочешь — оставь `default` и не дублируй Call отдельно. Главное чтобы не упало с unhandled kind.

- [ ] **Step 7.3: В `checkStmt` ДОБАВЬ ветку Require:**
```cpp
        case StmtKind::Require: {
            auto& r = static_cast<RequireStmt&>(s);
            if (!checkExpr(*r.cond)) return false;
            if (r.cond->resolvedType->kind != TypeKind::Bool) {
                diag_.error(r.loc, "require expects a bool condition");
                return false;
            }
            return true;
        }
```

- [ ] **Step 7.4: Build + test**
```bash
ninja unit_tests && ctest --output-on-failure
```
Expected: 75/75.

- [ ] **Step 7.5: Commit**
```bash
git commit -am "feat(sema): require statement bool check"
```

---

### Task 8: остальные statements (If, Return, VarDecl type-check, Assign type-check)

**Files:** Sema.h, Sema.cpp, test_sema.cpp.

Цель: дополнить `checkStmt` оставшимися кейсами и проверять совпадение типов в VarDecl/Assign/Return.

- [ ] **Step 8.1: Failing tests**
```cpp
TEST(Sema, VarDeclTypeMismatch) {
    solc::DiagnosticEngine d;
    solc::Lexer lex(
        "contract C { function f() public { uint256 a = true; } }", d);
    solc::Parser p(lex, d);
    auto c = p.parseContract();
    ASSERT_TRUE(c.has_value());
    solc::sema::SemanticAnalyzer s(d);
    EXPECT_FALSE(s.analyze(*c));
    EXPECT_TRUE(d.hasErrors());
}

TEST(Sema, ReturnTypeMismatch) {
    solc::DiagnosticEngine d;
    solc::Lexer lex(
        "contract C { function f() public returns (uint256) { return true; } }", d);
    solc::Parser p(lex, d);
    auto c = p.parseContract();
    ASSERT_TRUE(c.has_value());
    solc::sema::SemanticAnalyzer s(d);
    EXPECT_FALSE(s.analyze(*c));
    EXPECT_TRUE(d.hasErrors());
}

TEST(Sema, AssignTypeMismatch) {
    solc::DiagnosticEngine d;
    solc::Lexer lex(
        "contract C { uint256 v; function f() public { v = true; } }", d);
    solc::Parser p(lex, d);
    auto c = p.parseContract();
    ASSERT_TRUE(c.has_value());
    solc::sema::SemanticAnalyzer s(d);
    EXPECT_FALSE(s.analyze(*c));
    EXPECT_TRUE(d.hasErrors());
}

TEST(Sema, IfCondMustBeBool) {
    solc::DiagnosticEngine d;
    solc::Lexer lex(
        "contract C { function f() public { if (1) { } } }", d);
    solc::Parser p(lex, d);
    auto c = p.parseContract();
    ASSERT_TRUE(c.has_value());
    solc::sema::SemanticAnalyzer s(d);
    EXPECT_FALSE(s.analyze(*c));
    EXPECT_TRUE(d.hasErrors());
}
```

- [ ] **Step 8.2: В `analyzeFunction` нужен текущий expected return type**

Расширь `SemanticAnalyzer` приватным полем (или передавай параметром):
```cpp
    const ast::Type* currentReturnType_ = nullptr;
```

В `analyzeFunction` установи:
```cpp
    currentReturnType_ = fn.returnType ? &*fn.returnType : nullptr;
    // ...
    currentReturnType_ = nullptr;   // в конце
```

- [ ] **Step 8.3: Расширь `checkStmt`**

VarDecl: после `checkExpr(*v.init)` добавь:
```cpp
            if (v.init->resolvedType->kind != v.type.kind) {
                diag_.error(v.loc, "var-decl type mismatch");
                return false;
            }
```
(вставь ДО `symbols_.declare(...)`).

Assign: после lookup target'а добавь проверку типов:
```cpp
            Symbol* tsym = symbols_.lookup(a.target);
            if (!tsym) { diag_.error(a.loc, ...); return false; }
            if (!checkExpr(*a.value)) return false;
            if (a.value->resolvedType->kind != tsym->type.kind) {
                diag_.error(a.loc, "assignment type mismatch");
                return false;
            }
            return true;
```

If:
```cpp
        case StmtKind::If: {
            auto& i = static_cast<IfStmt&>(s);
            if (!checkExpr(*i.cond)) return false;
            if (i.cond->resolvedType->kind != TypeKind::Bool) {
                diag_.error(i.loc, "if condition must be bool");
                return false;
            }
            symbols_.enterScope();
            bool ok = true;
            for (auto& sp : i.thenBlock) if (!checkStmt(*sp)) ok = false;
            symbols_.leaveScope();
            symbols_.enterScope();
            for (auto& sp : i.elseBlock) if (!checkStmt(*sp)) ok = false;
            symbols_.leaveScope();
            return ok;
        }
```

Return:
```cpp
        case StmtKind::Return: {
            auto& r = static_cast<ReturnStmt&>(s);
            if (!r.value) {
                if (currentReturnType_) {
                    diag_.error(r.loc, "function expects a return value");
                    return false;
                }
                return true;
            }
            if (!checkExpr(*r.value)) return false;
            if (!currentReturnType_) {
                diag_.error(r.loc, "function does not return a value");
                return false;
            }
            if (r.value->resolvedType->kind != currentReturnType_->kind) {
                diag_.error(r.loc, "return type mismatch");
                return false;
            }
            return true;
        }
```

- [ ] **Step 8.4: Build + test**
```bash
ninja unit_tests && ctest --output-on-failure
```
Expected: 79/79.

- [ ] **Step 8.5: Commit**
```bash
git commit -am "feat(sema): statement type-checks (if/return/var-decl/assign)"
```

---

### Task 9: Полный Counter sema-clean

**Files:** test_sema.cpp.

- [ ] **Step 9.1: Failing test (вероятно сразу зелёный)**
```cpp
TEST(Sema, CounterFixturePassesSema) {
    std::ifstream f(std::string(SOLC_TESTS_DIR) + "/fixtures/counter.sol");
    std::stringstream ss; ss << f.rdbuf();
    std::string src = ss.str();
    ASSERT_FALSE(src.empty());

    solc::DiagnosticEngine d;
    solc::Lexer lex(src, d);
    solc::Parser p(lex, d);
    auto c = p.parseContract();
    ASSERT_TRUE(c.has_value());

    solc::sema::SemanticAnalyzer s(d);
    EXPECT_TRUE(s.analyze(*c));
    EXPECT_FALSE(d.hasErrors());

    EXPECT_EQ(c->stateVars[0].name, "value");
    EXPECT_EQ(c->stateVars[0].storageSlot, 0);
    EXPECT_EQ(c->stateVars[1].name, "owner");
    EXPECT_EQ(c->stateVars[1].storageSlot, 1);
}
```

(Понадобится `#include <fstream>`/`<sstream>` если ещё нет.)

- [ ] **Step 9.2: Build + test**
```bash
ninja unit_tests && ctest --output-on-failure
```
Expected: 80/80.

Если падает — отчитайся подробно (какая диагностика, на каком stmt).

- [ ] **Step 9.3: Commit**
```bash
git commit -am "test(sema): Counter.sol passes full sema with no diagnostics"
```

---

## Scope boundaries

- НЕ обрабатывать `CallExpr` как настоящий вызов — в MVP только `require` через `RequireStmt`.
- НЕ поддерживать общие member-accesses помимо `msg.sender`/`msg.value`.
- НЕ интегрировать Sema в Driver — это можно сделать на неделе 8 (CLI шлифовка). Сейчас Sema запускается только в тестах.
- НЕ менять Lexer/Parser/AST кроме добавления аннотаций.

---

## Self-review checklist

- [ ] Каждое expr-правило таблицы покрыто тестом
- [ ] msg.sender и msg.value возвращают правильные типы
- [ ] require требует bool
- [ ] state vars получают `storageSlot` по порядку
- [ ] параметры регистрируются в новом scope
- [ ] локальные не shadow'ят параметры в том же scope (проверь)
- [ ] Counter полностью чистый

---

## Критерий готовности недели 5

- [ ] ≥ 19 новых тестов sema (итого ≥ 77)
- [ ] `counter.sol` end-to-end через lexer → parser → sema — без единой диагностики
- [ ] state vars получили `storageSlot` 0, 1
- [ ] Хотя бы один негативный тест на каждое правило: undecl, undefined op typing, mismatch type-check
- [ ] Минимум 9 коммитов
