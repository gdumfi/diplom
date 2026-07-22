# Week 4 — Parser: Statements & Expressions

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development или superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Заполнить тело функций реальным AST: разбирать statements (`if/else`, `return`, `require`, var-decl, assign) и expressions (литералы, идентификаторы, member access, call, унарный `!`, бинарные операторы с правильным приоритетом). По итогам полный `counter.sol` парсится в AST с полностью раскрытыми телами обеих функций.

**Architecture:** Узлы AST для Expr и Stmt — class-hierarchy с тегом `kind` (паттерн «дискриминированное объединение», как в LLVM). Хранение через `std::unique_ptr<Expr>` / `std::unique_ptr<Stmt>` для рекурсии и владения. Парсер выражений — **precedence climbing** (рекурсивный спуск с параметром min-precedence). Парсер statements — switch по `peek().kind` в `parseStmt`. `Block` перестаёт быть заглушкой и теперь содержит `std::vector<StmtPtr> stmts`.

**Tech Stack:** C++20 (`std::unique_ptr`, `std::optional`, `std::variant` не используем), продолжение недели 3.

---

## File Structure

- **Modify:** `include/solc/ast/AST.h` — добавить Expr-узлы, Stmt-узлы, обновить `Block`.
- **Modify:** `src/ast/AST.cpp` — расширить `dump()` чтобы печатать тела функций; добавить хелперы `binOpName`, `unOpName`.
- **Modify:** `include/solc/parser/Parser.h` — публичные `parseExpr`, `parseStmt`, `parseBlock`; приватные `parsePrimary`, `parseUnary`, `parseBinary(int minPrec)`, `binOpPrecedence`, `tokenToBinOp`.
- **Modify:** `src/parser/Parser.cpp` — реализация expr/stmt парсеров; `parseFunctionDecl` теперь вызывает `parseBlock` вместо `skipBraceBody`.
- **Modify:** `tests/test_parser.cpp` — ~15 новых TEST.

Удалять `skipBraceBody`/`synchronizeToBraceEnd` НЕ нужно — `skipBraceBody` остаётся для error recovery, `synchronizeToBraceEnd` тоже.

---

## AST design (фиксируем на старте)

### Expressions

```cpp
namespace solc::ast {

enum class ExprKind { NumberLit, BoolLit, Ident, Binary, Unary, Member, Call };

struct Expr {
    ExprKind  kind;
    SourceLoc loc;
    Expr(ExprKind k, SourceLoc l) : kind(k), loc(l) {}
    virtual ~Expr() = default;        // нужен для корректного delete через ExprPtr
};
using ExprPtr = std::unique_ptr<Expr>;

struct NumberLit : Expr {
    std::string text;                 // храним как строку, парсить в число — в codegen
    NumberLit(std::string t, SourceLoc l)
        : Expr(ExprKind::NumberLit, l), text(std::move(t)) {}
};

struct BoolLit : Expr {
    bool value;
    BoolLit(bool v, SourceLoc l) : Expr(ExprKind::BoolLit, l), value(v) {}
};

struct IdentExpr : Expr {
    std::string name;
    IdentExpr(std::string n, SourceLoc l)
        : Expr(ExprKind::Ident, l), name(std::move(n)) {}
};

enum class BinOp {
    Add, Sub, Mul, Div,
    Eq, NotEq, Lt, Gt, LtEq, GtEq,
    AndAnd, OrOr,
};

struct BinaryExpr : Expr {
    BinOp   op;
    ExprPtr lhs;
    ExprPtr rhs;
    BinaryExpr(BinOp o, ExprPtr l, ExprPtr r, SourceLoc loc)
        : Expr(ExprKind::Binary, loc), op(o),
          lhs(std::move(l)), rhs(std::move(r)) {}
};

enum class UnOp { Not };

struct UnaryExpr : Expr {
    UnOp    op;
    ExprPtr operand;
    UnaryExpr(UnOp o, ExprPtr e, SourceLoc loc)
        : Expr(ExprKind::Unary, loc), op(o), operand(std::move(e)) {}
};

struct MemberExpr : Expr {
    ExprPtr     base;
    std::string member;
    MemberExpr(ExprPtr b, std::string m, SourceLoc loc)
        : Expr(ExprKind::Member, loc),
          base(std::move(b)), member(std::move(m)) {}
};

struct CallExpr : Expr {
    ExprPtr              callee;
    std::vector<ExprPtr> args;
    CallExpr(ExprPtr c, std::vector<ExprPtr> a, SourceLoc loc)
        : Expr(ExprKind::Call, loc),
          callee(std::move(c)), args(std::move(a)) {}
};

const char* binOpName(BinOp op);
const char* unOpName(UnOp op);

}  // namespace solc::ast
```

### Statements

```cpp
namespace solc::ast {

enum class StmtKind { VarDecl, Assign, If, Return, Require };

struct Stmt {
    StmtKind  kind;
    SourceLoc loc;
    Stmt(StmtKind k, SourceLoc l) : kind(k), loc(l) {}
    virtual ~Stmt() = default;
};
using StmtPtr = std::unique_ptr<Stmt>;

struct VarDeclStmt : Stmt {
    Type        type;
    std::string name;
    ExprPtr     init;       // в MVP всегда есть инициализатор
    VarDeclStmt(Type t, std::string n, ExprPtr i, SourceLoc loc)
        : Stmt(StmtKind::VarDecl, loc),
          type(t), name(std::move(n)), init(std::move(i)) {}
};

struct AssignStmt : Stmt {
    std::string target;     // только Identifier = expr в MVP
    ExprPtr     value;
    AssignStmt(std::string t, ExprPtr v, SourceLoc loc)
        : Stmt(StmtKind::Assign, loc),
          target(std::move(t)), value(std::move(v)) {}
};

struct IfStmt : Stmt {
    ExprPtr               cond;
    std::vector<StmtPtr>  thenBlock;
    std::vector<StmtPtr>  elseBlock;   // пустой если нет else
    IfStmt(ExprPtr c, std::vector<StmtPtr> t,
           std::vector<StmtPtr> e, SourceLoc loc)
        : Stmt(StmtKind::If, loc),
          cond(std::move(c)),
          thenBlock(std::move(t)),
          elseBlock(std::move(e)) {}
};

struct ReturnStmt : Stmt {
    ExprPtr value;   // может быть nullptr (в MVP всегда есть, но допустим)
    ReturnStmt(ExprPtr v, SourceLoc loc)
        : Stmt(StmtKind::Return, loc), value(std::move(v)) {}
};

struct RequireStmt : Stmt {
    ExprPtr cond;
    RequireStmt(ExprPtr c, SourceLoc loc)
        : Stmt(StmtKind::Require, loc), cond(std::move(c)) {}
};

}  // namespace solc::ast
```

### Block — теперь содержит statements

```cpp
struct Block {
    std::vector<StmtPtr> stmts;
    SourceLoc            loc;
};
```

**Важно:** `Block` теперь **move-only** (vector от unique_ptr). Каскадно `FunctionDecl` тоже становится move-only. В `Parser.cpp` строка `fn.body = body;` должна стать `fn.body = std::move(body);`. `std::vector<FunctionDecl>` это поддерживает.

---

## Precedence table (от низкого к высокому)

| Уровень | Операторы | Ассоциативность |
|---|---|---|
| 1 | `\|\|` | левая |
| 2 | `&&` | левая |
| 3 | `==`, `!=` | левая |
| 4 | `<`, `>`, `<=`, `>=` | левая |
| 5 | `+`, `-` | левая |
| 6 | `*`, `/` | левая |
| 7 (унарный) | `!` | префиксный |
| 8 (postfix) | `.member`, `(args)` | левая |
| 9 (primary) | literal, ident, `(expr)` | — |

`parseBinary(int minPrec)`: рекурсивный спуск с climbing — для каждого peeked бинарного оператора с precedence ≥ minPrec потребить его и рекурсивно вызвать `parseBinary(prec+1)` для правого операнда (левая ассоциативность).

---

## Tasks

### Task 1: AST Expr-узлы и хелперы dump

**Files:**
- Modify: `include/solc/ast/AST.h`
- Modify: `src/ast/AST.cpp`
- Modify: `tests/test_parser.cpp`

- [ ] **Step 1.1: Failing test (построение AST вручную + dump)**

Добавь в `tests/test_parser.cpp` (после существующих includes):

```cpp
TEST(AST, ExprConstructionAndKinds) {
    using namespace solc::ast;
    auto n = std::make_unique<NumberLit>("42", solc::SourceLoc{1, 1});
    EXPECT_EQ(n->kind, ExprKind::NumberLit);
    EXPECT_EQ(n->text, "42");

    auto b = std::make_unique<BoolLit>(true, solc::SourceLoc{1, 1});
    EXPECT_EQ(b->kind, ExprKind::BoolLit);
    EXPECT_TRUE(b->value);

    auto lhs = std::make_unique<NumberLit>("1", solc::SourceLoc{1, 1});
    auto rhs = std::make_unique<NumberLit>("2", solc::SourceLoc{1, 5});
    auto bin = std::make_unique<BinaryExpr>(
        BinOp::Add, std::move(lhs), std::move(rhs), solc::SourceLoc{1, 1});
    EXPECT_EQ(bin->kind, ExprKind::Binary);
    EXPECT_EQ(bin->op, BinOp::Add);
}
```

- [ ] **Step 1.2: Compile fails (типы ещё не объявлены) — это ожидаемое RED**

```bash
cd /home/akum1/diplom/solidity-llvm/build
ninja unit_tests
```
Expected: ошибка компиляции про неизвестные имена.

- [ ] **Step 1.3: Добавь Expr-узлы в `include/solc/ast/AST.h`**

В существующий namespace `solc::ast` добавь **ПЕРЕД** объявлениями `Param/StateVarDecl/FunctionDecl/ContractDecl`:

```cpp
enum class ExprKind { NumberLit, BoolLit, Ident, Binary, Unary, Member, Call };

struct Expr {
    ExprKind  kind;
    SourceLoc loc;
    Expr(ExprKind k, SourceLoc l) : kind(k), loc(l) {}
    virtual ~Expr() = default;
};
using ExprPtr = std::unique_ptr<Expr>;

struct NumberLit : Expr {
    std::string text;
    NumberLit(std::string t, SourceLoc l)
        : Expr(ExprKind::NumberLit, l), text(std::move(t)) {}
};

struct BoolLit : Expr {
    bool value;
    BoolLit(bool v, SourceLoc l) : Expr(ExprKind::BoolLit, l), value(v) {}
};

struct IdentExpr : Expr {
    std::string name;
    IdentExpr(std::string n, SourceLoc l)
        : Expr(ExprKind::Ident, l), name(std::move(n)) {}
};

enum class BinOp {
    Add, Sub, Mul, Div,
    Eq, NotEq, Lt, Gt, LtEq, GtEq,
    AndAnd, OrOr,
};

struct BinaryExpr : Expr {
    BinOp   op;
    ExprPtr lhs;
    ExprPtr rhs;
    BinaryExpr(BinOp o, ExprPtr l, ExprPtr r, SourceLoc loc)
        : Expr(ExprKind::Binary, loc),
          op(o), lhs(std::move(l)), rhs(std::move(r)) {}
};

enum class UnOp { Not };

struct UnaryExpr : Expr {
    UnOp    op;
    ExprPtr operand;
    UnaryExpr(UnOp o, ExprPtr e, SourceLoc loc)
        : Expr(ExprKind::Unary, loc), op(o), operand(std::move(e)) {}
};

struct MemberExpr : Expr {
    ExprPtr     base;
    std::string member;
    MemberExpr(ExprPtr b, std::string m, SourceLoc loc)
        : Expr(ExprKind::Member, loc),
          base(std::move(b)), member(std::move(m)) {}
};

struct CallExpr : Expr {
    ExprPtr              callee;
    std::vector<ExprPtr> args;
    CallExpr(ExprPtr c, std::vector<ExprPtr> a, SourceLoc loc)
        : Expr(ExprKind::Call, loc),
          callee(std::move(c)), args(std::move(a)) {}
};

const char* binOpName(BinOp op);
const char* unOpName(UnOp op);
```

Также в самый верх AST.h добавить (если ещё нет):
```cpp
#include <memory>
```

- [ ] **Step 1.4: Реализуй `binOpName`/`unOpName` в `src/ast/AST.cpp`**

В namespace `solc::ast` добавь:

```cpp
const char* binOpName(BinOp op) {
    switch (op) {
        case BinOp::Add:    return "+";
        case BinOp::Sub:    return "-";
        case BinOp::Mul:    return "*";
        case BinOp::Div:    return "/";
        case BinOp::Eq:     return "==";
        case BinOp::NotEq:  return "!=";
        case BinOp::Lt:     return "<";
        case BinOp::Gt:     return ">";
        case BinOp::LtEq:   return "<=";
        case BinOp::GtEq:   return ">=";
        case BinOp::AndAnd: return "&&";
        case BinOp::OrOr:   return "||";
    }
    return "?";
}

const char* unOpName(UnOp op) {
    switch (op) {
        case UnOp::Not: return "!";
    }
    return "?";
}
```

- [ ] **Step 1.5: Build + run test**
```bash
ninja unit_tests && ctest -R AST.ExprConstructionAndKinds --output-on-failure
```
Expected: PASS. Полный suite: 36/36.

- [ ] **Step 1.6: Commit**
```bash
git add include/solc/ast/AST.h src/ast/AST.cpp tests/test_parser.cpp
git commit -m "feat(ast): expression nodes (Number/Bool/Ident/Binary/Unary/Member/Call)"
```

---

### Task 2: AST Stmt-узлы + обновлённый Block

**Files:** те же.

- [ ] **Step 2.1: Failing test для построения Stmt**

Добавь в `tests/test_parser.cpp`:

```cpp
TEST(AST, StmtConstructionAndKinds) {
    using namespace solc::ast;
    auto v = std::make_unique<NumberLit>("42", solc::SourceLoc{});
    auto vd = std::make_unique<VarDeclStmt>(
        Type{TypeKind::Uint256, {}}, "x", std::move(v), solc::SourceLoc{});
    EXPECT_EQ(vd->kind, StmtKind::VarDecl);
    EXPECT_EQ(vd->name, "x");
    EXPECT_EQ(vd->type.kind, TypeKind::Uint256);
    ASSERT_NE(vd->init, nullptr);
    EXPECT_EQ(vd->init->kind, ExprKind::NumberLit);
}

TEST(AST, BlockHoldsStatements) {
    using namespace solc::ast;
    Block b;
    auto val = std::make_unique<NumberLit>("1", solc::SourceLoc{});
    b.stmts.push_back(std::make_unique<ReturnStmt>(std::move(val), solc::SourceLoc{}));
    ASSERT_EQ(b.stmts.size(), 1u);
    EXPECT_EQ(b.stmts[0]->kind, StmtKind::Return);
}
```

- [ ] **Step 2.2: Compile fails — типов нет**

```bash
ninja unit_tests
```

- [ ] **Step 2.3: Добавь Stmt-узлы в `AST.h`** (после Expr-узлов, до `Param`):

```cpp
enum class StmtKind { VarDecl, Assign, If, Return, Require };

struct Stmt {
    StmtKind  kind;
    SourceLoc loc;
    Stmt(StmtKind k, SourceLoc l) : kind(k), loc(l) {}
    virtual ~Stmt() = default;
};
using StmtPtr = std::unique_ptr<Stmt>;

struct VarDeclStmt : Stmt {
    Type        type;
    std::string name;
    ExprPtr     init;
    VarDeclStmt(Type t, std::string n, ExprPtr i, SourceLoc loc)
        : Stmt(StmtKind::VarDecl, loc),
          type(t), name(std::move(n)), init(std::move(i)) {}
};

struct AssignStmt : Stmt {
    std::string target;
    ExprPtr     value;
    AssignStmt(std::string t, ExprPtr v, SourceLoc loc)
        : Stmt(StmtKind::Assign, loc),
          target(std::move(t)), value(std::move(v)) {}
};

struct IfStmt : Stmt {
    ExprPtr               cond;
    std::vector<StmtPtr>  thenBlock;
    std::vector<StmtPtr>  elseBlock;
    IfStmt(ExprPtr c, std::vector<StmtPtr> t,
           std::vector<StmtPtr> e, SourceLoc loc)
        : Stmt(StmtKind::If, loc),
          cond(std::move(c)),
          thenBlock(std::move(t)),
          elseBlock(std::move(e)) {}
};

struct ReturnStmt : Stmt {
    ExprPtr value;
    ReturnStmt(ExprPtr v, SourceLoc loc)
        : Stmt(StmtKind::Return, loc), value(std::move(v)) {}
};

struct RequireStmt : Stmt {
    ExprPtr cond;
    RequireStmt(ExprPtr c, SourceLoc loc)
        : Stmt(StmtKind::Require, loc), cond(std::move(c)) {}
};
```

- [ ] **Step 2.4: Обнови `Block` (там же в AST.h)**

Замени:
```cpp
struct Block {
    SourceLoc loc;
};
```
на:
```cpp
struct Block {
    std::vector<StmtPtr> stmts;
    SourceLoc            loc;
};
```

- [ ] **Step 2.5: Поправь `Parser::parseFunctionDecl`**

Найди в `src/parser/Parser.cpp` в конце `parseFunctionDecl`:
```cpp
    ast::FunctionDecl fn;
    fn.name       = std::move(name);
    fn.params     = std::move(params);
    fn.returnType = retType;
    fn.body       = body;          // <- эта строка
    fn.loc        = startLoc;
    return fn;
```
Замени `fn.body = body;` на `fn.body = std::move(body);`. После этого `body` уже не используется — ок.

- [ ] **Step 2.6: Build + test**
```bash
ninja unit_tests && ctest --output-on-failure
```
Expected: 38/38 (36 + 2 новых). Регрессий быть не должно — `Parser.CounterFixtureFullDeclarations` и т.п. продолжают работать, т.к. тело пока ещё пропускается через `skipBraceBody`.

**Если `Parser.CounterFixtureFullDeclarations` падает на этапе `fn.body = std::move(body);` с ошибкой про deleted copy** — значит ещё где-то осталось копирование `Block`. Найди и поправь.

- [ ] **Step 2.7: Commit**
```bash
git commit -am "feat(ast): statement nodes + Block with stmt vector"
```

---

### Task 3: parsePrimary — литералы, идентификаторы, скобки, member access, call

**Files:**
- Modify: `include/solc/parser/Parser.h`
- Modify: `src/parser/Parser.cpp`
- Modify: `tests/test_parser.cpp`

- [ ] **Step 3.1: Failing tests**

Добавь в `tests/test_parser.cpp`:

```cpp
TEST(Parser, ParsePrimaryNumberLit) {
    solc::DiagnosticEngine d;
    solc::Lexer lex("42", d);
    solc::Parser p(lex, d);
    auto e = p.parseExpr();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(e->kind, solc::ast::ExprKind::NumberLit);
    EXPECT_EQ(static_cast<solc::ast::NumberLit*>(e.get())->text, "42");
}

TEST(Parser, ParsePrimaryBoolLit) {
    solc::DiagnosticEngine d;
    solc::Lexer lex("true", d);
    solc::Parser p(lex, d);
    auto e = p.parseExpr();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(e->kind, solc::ast::ExprKind::BoolLit);
    EXPECT_TRUE(static_cast<solc::ast::BoolLit*>(e.get())->value);
}

TEST(Parser, ParsePrimaryIdent) {
    solc::DiagnosticEngine d;
    solc::Lexer lex("foo", d);
    solc::Parser p(lex, d);
    auto e = p.parseExpr();
    ASSERT_EQ(e->kind, solc::ast::ExprKind::Ident);
    EXPECT_EQ(static_cast<solc::ast::IdentExpr*>(e.get())->name, "foo");
}

TEST(Parser, ParsePrimaryParenthesized) {
    solc::DiagnosticEngine d;
    solc::Lexer lex("(123)", d);
    solc::Parser p(lex, d);
    auto e = p.parseExpr();
    ASSERT_EQ(e->kind, solc::ast::ExprKind::NumberLit);
    EXPECT_EQ(static_cast<solc::ast::NumberLit*>(e.get())->text, "123");
}

TEST(Parser, ParseMemberAccess) {
    solc::DiagnosticEngine d;
    solc::Lexer lex("msg.sender", d);
    solc::Parser p(lex, d);
    auto e = p.parseExpr();
    ASSERT_EQ(e->kind, solc::ast::ExprKind::Member);
    auto* m = static_cast<solc::ast::MemberExpr*>(e.get());
    EXPECT_EQ(m->member, "sender");
    ASSERT_EQ(m->base->kind, solc::ast::ExprKind::Ident);
    EXPECT_EQ(static_cast<solc::ast::IdentExpr*>(m->base.get())->name, "msg");
}

TEST(Parser, ParseCallNoArgs) {
    solc::DiagnosticEngine d;
    solc::Lexer lex("f()", d);
    solc::Parser p(lex, d);
    auto e = p.parseExpr();
    ASSERT_EQ(e->kind, solc::ast::ExprKind::Call);
    auto* c = static_cast<solc::ast::CallExpr*>(e.get());
    EXPECT_TRUE(c->args.empty());
    ASSERT_EQ(c->callee->kind, solc::ast::ExprKind::Ident);
}

TEST(Parser, ParseCallTwoArgs) {
    solc::DiagnosticEngine d;
    solc::Lexer lex("f(1, 2)", d);
    solc::Parser p(lex, d);
    auto e = p.parseExpr();
    ASSERT_EQ(e->kind, solc::ast::ExprKind::Call);
    auto* c = static_cast<solc::ast::CallExpr*>(e.get());
    ASSERT_EQ(c->args.size(), 2u);
    EXPECT_EQ(static_cast<solc::ast::NumberLit*>(c->args[0].get())->text, "1");
    EXPECT_EQ(static_cast<solc::ast::NumberLit*>(c->args[1].get())->text, "2");
}
```

- [ ] **Step 3.2: Расширь `Parser.h`**

В публичную секцию добавь:
```cpp
    ast::ExprPtr parseExpr();      // entry-point для теста и стейтментов
```

В приватную секцию:
```cpp
    ast::ExprPtr parsePrimary();
```

- [ ] **Step 3.3: Реализуй `parsePrimary` и `parseExpr` (пока без бинарных операторов)**

В `src/parser/Parser.cpp` добавь:

```cpp
ast::ExprPtr Parser::parsePrimary() {
    SourceLoc loc = peek().loc;
    switch (peek().kind) {
        case TokenKind::Number: {
            std::string text(peek().lexeme);
            advance();
            return std::make_unique<ast::NumberLit>(std::move(text), loc);
        }
        case TokenKind::True:
            advance();
            return std::make_unique<ast::BoolLit>(true, loc);
        case TokenKind::False:
            advance();
            return std::make_unique<ast::BoolLit>(false, loc);
        case TokenKind::Identifier: {
            std::string name(peek().lexeme);
            advance();
            ast::ExprPtr e = std::make_unique<ast::IdentExpr>(std::move(name), loc);
            // postfix: .member и (args)
            for (;;) {
                if (match(TokenKind::Dot)) {
                    if (!check(TokenKind::Identifier)) {
                        diag_.error(peek().loc, "expected member name after '.'");
                        return nullptr;
                    }
                    std::string m(peek().lexeme);
                    SourceLoc mloc = peek().loc;
                    advance();
                    e = std::make_unique<ast::MemberExpr>(std::move(e), std::move(m), mloc);
                } else if (match(TokenKind::LParen)) {
                    std::vector<ast::ExprPtr> args;
                    if (!check(TokenKind::RParen)) {
                        for (;;) {
                            auto a = parseExpr();
                            if (!a) return nullptr;
                            args.push_back(std::move(a));
                            if (!match(TokenKind::Comma)) break;
                        }
                    }
                    if (!expect(TokenKind::RParen, "call argument list")) return nullptr;
                    e = std::make_unique<ast::CallExpr>(std::move(e), std::move(args), loc);
                } else {
                    break;
                }
            }
            return e;
        }
        case TokenKind::LParen: {
            advance();
            auto inner = parseExpr();
            if (!inner) return nullptr;
            if (!expect(TokenKind::RParen, "parenthesized expression")) return nullptr;
            return inner;
        }
        default:
            diag_.error(loc, std::string("unexpected token in expression: ")
                                 + tokenKindName(peek().kind));
            return nullptr;
    }
}

ast::ExprPtr Parser::parseExpr() {
    return parsePrimary();   // на этом шаге без бинарных/унарных — добавим в Task 4-5
}
```

- [ ] **Step 3.4: Build + test**
```bash
ninja unit_tests && ctest --output-on-failure
```
Expected: 45/45 (38 + 7 новых).

- [ ] **Step 3.5: Commit**
```bash
git commit -am "feat(parser): parsePrimary — literals, idents, member, call, parens"
```

---

### Task 4: unary `!` + binary precedence climbing

**Files:** Parser.h, Parser.cpp, test_parser.cpp.

- [ ] **Step 4.1: Failing tests**

Добавь в `test_parser.cpp`:

```cpp
TEST(Parser, ParseUnaryNot) {
    solc::DiagnosticEngine d;
    solc::Lexer lex("!x", d);
    solc::Parser p(lex, d);
    auto e = p.parseExpr();
    ASSERT_EQ(e->kind, solc::ast::ExprKind::Unary);
    auto* u = static_cast<solc::ast::UnaryExpr*>(e.get());
    EXPECT_EQ(u->op, solc::ast::UnOp::Not);
    EXPECT_EQ(u->operand->kind, solc::ast::ExprKind::Ident);
}

TEST(Parser, ParseAddMulPrecedence) {
    // 1 + 2 * 3 → (1 + (2 * 3))
    solc::DiagnosticEngine d;
    solc::Lexer lex("1 + 2 * 3", d);
    solc::Parser p(lex, d);
    auto e = p.parseExpr();
    ASSERT_EQ(e->kind, solc::ast::ExprKind::Binary);
    auto* outer = static_cast<solc::ast::BinaryExpr*>(e.get());
    EXPECT_EQ(outer->op, solc::ast::BinOp::Add);
    EXPECT_EQ(outer->lhs->kind, solc::ast::ExprKind::NumberLit);
    ASSERT_EQ(outer->rhs->kind, solc::ast::ExprKind::Binary);
    auto* inner = static_cast<solc::ast::BinaryExpr*>(outer->rhs.get());
    EXPECT_EQ(inner->op, solc::ast::BinOp::Mul);
}

TEST(Parser, ParseEqAndAssociativity) {
    // a == b && c → ((a == b) && c)
    solc::DiagnosticEngine d;
    solc::Lexer lex("a == b && c", d);
    solc::Parser p(lex, d);
    auto e = p.parseExpr();
    ASSERT_EQ(e->kind, solc::ast::ExprKind::Binary);
    auto* outer = static_cast<solc::ast::BinaryExpr*>(e.get());
    EXPECT_EQ(outer->op, solc::ast::BinOp::AndAnd);
    ASSERT_EQ(outer->lhs->kind, solc::ast::ExprKind::Binary);
    auto* lhs = static_cast<solc::ast::BinaryExpr*>(outer->lhs.get());
    EXPECT_EQ(lhs->op, solc::ast::BinOp::Eq);
    EXPECT_EQ(outer->rhs->kind, solc::ast::ExprKind::Ident);
}

TEST(Parser, ParseUnaryOverBinary) {
    // !x || y → ((!x) || y)
    solc::DiagnosticEngine d;
    solc::Lexer lex("!x || y", d);
    solc::Parser p(lex, d);
    auto e = p.parseExpr();
    ASSERT_EQ(e->kind, solc::ast::ExprKind::Binary);
    auto* outer = static_cast<solc::ast::BinaryExpr*>(e.get());
    EXPECT_EQ(outer->op, solc::ast::BinOp::OrOr);
    EXPECT_EQ(outer->lhs->kind, solc::ast::ExprKind::Unary);
}

TEST(Parser, LeftAssociativity) {
    // 1 - 2 - 3 → ((1 - 2) - 3)
    solc::DiagnosticEngine d;
    solc::Lexer lex("1 - 2 - 3", d);
    solc::Parser p(lex, d);
    auto e = p.parseExpr();
    ASSERT_EQ(e->kind, solc::ast::ExprKind::Binary);
    auto* outer = static_cast<solc::ast::BinaryExpr*>(e.get());
    EXPECT_EQ(outer->op, solc::ast::BinOp::Sub);
    EXPECT_EQ(outer->rhs->kind, solc::ast::ExprKind::NumberLit);
    ASSERT_EQ(outer->lhs->kind, solc::ast::ExprKind::Binary);
}
```

- [ ] **Step 4.2: Расширь `Parser.h`**

В приватную секцию:
```cpp
    ast::ExprPtr parseUnary();
    ast::ExprPtr parseBinary(int minPrec);

    static int                              binOpPrecedence(TokenKind k);
    static std::optional<ast::BinOp>        tokenToBinOp(TokenKind k);
```

- [ ] **Step 4.3: Реализация**

В `Parser.cpp`:

```cpp
int Parser::binOpPrecedence(TokenKind k) {
    switch (k) {
        case TokenKind::OrOr:                                       return 1;
        case TokenKind::AndAnd:                                     return 2;
        case TokenKind::EqEq:  case TokenKind::NotEq:               return 3;
        case TokenKind::Lt:    case TokenKind::Gt:
        case TokenKind::LtEq:  case TokenKind::GtEq:                return 4;
        case TokenKind::Plus:  case TokenKind::Minus:               return 5;
        case TokenKind::Star:  case TokenKind::Slash:               return 6;
        default:                                                    return 0;
    }
}

std::optional<ast::BinOp> Parser::tokenToBinOp(TokenKind k) {
    switch (k) {
        case TokenKind::Plus:    return ast::BinOp::Add;
        case TokenKind::Minus:   return ast::BinOp::Sub;
        case TokenKind::Star:    return ast::BinOp::Mul;
        case TokenKind::Slash:   return ast::BinOp::Div;
        case TokenKind::EqEq:    return ast::BinOp::Eq;
        case TokenKind::NotEq:   return ast::BinOp::NotEq;
        case TokenKind::Lt:      return ast::BinOp::Lt;
        case TokenKind::Gt:      return ast::BinOp::Gt;
        case TokenKind::LtEq:    return ast::BinOp::LtEq;
        case TokenKind::GtEq:    return ast::BinOp::GtEq;
        case TokenKind::AndAnd:  return ast::BinOp::AndAnd;
        case TokenKind::OrOr:    return ast::BinOp::OrOr;
        default:                 return std::nullopt;
    }
}

ast::ExprPtr Parser::parseUnary() {
    if (check(TokenKind::Not)) {
        SourceLoc loc = peek().loc;
        advance();
        auto inner = parseUnary();
        if (!inner) return nullptr;
        return std::make_unique<ast::UnaryExpr>(ast::UnOp::Not, std::move(inner), loc);
    }
    return parsePrimary();
}

ast::ExprPtr Parser::parseBinary(int minPrec) {
    auto lhs = parseUnary();
    if (!lhs) return nullptr;
    for (;;) {
        int prec = binOpPrecedence(peek().kind);
        if (prec < minPrec || prec == 0) return lhs;

        auto op = tokenToBinOp(peek().kind);
        if (!op) return lhs;          // не бинарный — выходим
        SourceLoc loc = peek().loc;
        advance();

        auto rhs = parseBinary(prec + 1);    // левая ассоциативность
        if (!rhs) return nullptr;
        lhs = std::make_unique<ast::BinaryExpr>(*op, std::move(lhs), std::move(rhs), loc);
    }
}
```

Замени `parseExpr()`:
```cpp
ast::ExprPtr Parser::parseExpr() {
    return parseBinary(1);
}
```

- [ ] **Step 4.4: Build + test**
```bash
ninja unit_tests && ctest --output-on-failure
```
Expected: 50/50 (45 + 5 новых). **Все** существующие primary-тесты (Task 3) тоже должны пройти — они теперь идут через `parseBinary(1) → parseUnary → parsePrimary`.

- [ ] **Step 4.5: Commit**
```bash
git commit -am "feat(parser): unary ! and binary operators with precedence climbing"
```

---

### Task 5: parseStmt — return, require, var-decl, assign

**Files:** Parser.h, Parser.cpp, test_parser.cpp.

- [ ] **Step 5.1: Failing tests**

```cpp
TEST(Parser, StmtReturnExpr) {
    solc::DiagnosticEngine d;
    solc::Lexer lex("return x + 1;", d);
    solc::Parser p(lex, d);
    auto s = p.parseStmt();
    ASSERT_NE(s, nullptr);
    ASSERT_EQ(s->kind, solc::ast::StmtKind::Return);
    auto* r = static_cast<solc::ast::ReturnStmt*>(s.get());
    ASSERT_NE(r->value, nullptr);
    EXPECT_EQ(r->value->kind, solc::ast::ExprKind::Binary);
}

TEST(Parser, StmtRequire) {
    solc::DiagnosticEngine d;
    solc::Lexer lex("require(a == b);", d);
    solc::Parser p(lex, d);
    auto s = p.parseStmt();
    ASSERT_EQ(s->kind, solc::ast::StmtKind::Require);
    auto* r = static_cast<solc::ast::RequireStmt*>(s.get());
    EXPECT_EQ(r->cond->kind, solc::ast::ExprKind::Binary);
}

TEST(Parser, StmtVarDecl) {
    solc::DiagnosticEngine d;
    solc::Lexer lex("uint256 y = 42;", d);
    solc::Parser p(lex, d);
    auto s = p.parseStmt();
    ASSERT_EQ(s->kind, solc::ast::StmtKind::VarDecl);
    auto* v = static_cast<solc::ast::VarDeclStmt*>(s.get());
    EXPECT_EQ(v->type.kind, solc::ast::TypeKind::Uint256);
    EXPECT_EQ(v->name, "y");
    EXPECT_EQ(v->init->kind, solc::ast::ExprKind::NumberLit);
}

TEST(Parser, StmtAssign) {
    solc::DiagnosticEngine d;
    solc::Lexer lex("y = 100;", d);
    solc::Parser p(lex, d);
    auto s = p.parseStmt();
    ASSERT_EQ(s->kind, solc::ast::StmtKind::Assign);
    auto* a = static_cast<solc::ast::AssignStmt*>(s.get());
    EXPECT_EQ(a->target, "y");
    EXPECT_EQ(a->value->kind, solc::ast::ExprKind::NumberLit);
}
```

- [ ] **Step 5.2: Расширь `Parser.h`**

В публичной секции:
```cpp
    ast::StmtPtr parseStmt();
```

- [ ] **Step 5.3: Реализация в `Parser.cpp`**

```cpp
ast::StmtPtr Parser::parseStmt() {
    SourceLoc startLoc = peek().loc;

    // return expr;
    if (check(TokenKind::KwReturn)) {
        advance();
        auto val = parseExpr();
        if (!val) return nullptr;
        if (!expect(TokenKind::Semicolon, "return statement")) return nullptr;
        return std::make_unique<ast::ReturnStmt>(std::move(val), startLoc);
    }

    // require(expr);
    if (check(TokenKind::KwRequire)) {
        advance();
        if (!expect(TokenKind::LParen, "require statement")) return nullptr;
        auto cond = parseExpr();
        if (!cond) return nullptr;
        if (!expect(TokenKind::RParen, "require statement")) return nullptr;
        if (!expect(TokenKind::Semicolon, "require statement")) return nullptr;
        return std::make_unique<ast::RequireStmt>(std::move(cond), startLoc);
    }

    // type ident = expr;
    if (check(TokenKind::KwUint256) || check(TokenKind::KwBool) || check(TokenKind::KwAddress)) {
        auto ty = parseType();
        if (!ty) return nullptr;
        if (!check(TokenKind::Identifier)) {
            diag_.error(peek().loc, "expected variable name");
            return nullptr;
        }
        std::string name(peek().lexeme);
        advance();
        if (!expect(TokenKind::Eq, "variable declaration")) return nullptr;
        auto init = parseExpr();
        if (!init) return nullptr;
        if (!expect(TokenKind::Semicolon, "variable declaration")) return nullptr;
        return std::make_unique<ast::VarDeclStmt>(*ty, std::move(name), std::move(init), startLoc);
    }

    // ident = expr;
    if (check(TokenKind::Identifier)) {
        std::string target(peek().lexeme);
        advance();
        if (!expect(TokenKind::Eq, "assignment")) return nullptr;
        auto val = parseExpr();
        if (!val) return nullptr;
        if (!expect(TokenKind::Semicolon, "assignment")) return nullptr;
        return std::make_unique<ast::AssignStmt>(std::move(target), std::move(val), startLoc);
    }

    diag_.error(peek().loc, std::string("unexpected token in statement: ")
                                + tokenKindName(peek().kind));
    return nullptr;
}
```

- [ ] **Step 5.4: Build + test**
```bash
ninja unit_tests && ctest --output-on-failure
```
Expected: 54/54.

- [ ] **Step 5.5: Commit**
```bash
git commit -am "feat(parser): statements — return/require/var-decl/assign"
```

---

### Task 6: if / else

**Files:** Parser.h, Parser.cpp, test_parser.cpp.

- [ ] **Step 6.1: Failing tests**

```cpp
TEST(Parser, IfStmtNoElse) {
    solc::DiagnosticEngine d;
    solc::Lexer lex("if (x) { return 1; }", d);
    solc::Parser p(lex, d);
    auto s = p.parseStmt();
    ASSERT_EQ(s->kind, solc::ast::StmtKind::If);
    auto* i = static_cast<solc::ast::IfStmt*>(s.get());
    ASSERT_EQ(i->thenBlock.size(), 1u);
    EXPECT_EQ(i->thenBlock[0]->kind, solc::ast::StmtKind::Return);
    EXPECT_TRUE(i->elseBlock.empty());
}

TEST(Parser, IfStmtWithElse) {
    solc::DiagnosticEngine d;
    solc::Lexer lex("if (x) { return 1; } else { return 2; }", d);
    solc::Parser p(lex, d);
    auto s = p.parseStmt();
    ASSERT_EQ(s->kind, solc::ast::StmtKind::If);
    auto* i = static_cast<solc::ast::IfStmt*>(s.get());
    ASSERT_EQ(i->thenBlock.size(), 1u);
    ASSERT_EQ(i->elseBlock.size(), 1u);
}
```

- [ ] **Step 6.2: Расширь `Parser.h`**

В приватной секции:
```cpp
    bool parseBlockStatements(std::vector<ast::StmtPtr>& out);
```

- [ ] **Step 6.3: Реализация — `parseBlockStatements` и расширенный `parseStmt`**

В `Parser.cpp`:

```cpp
bool Parser::parseBlockStatements(std::vector<ast::StmtPtr>& out) {
    if (!expect(TokenKind::LBrace, "block")) return false;
    while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
        auto s = parseStmt();
        if (s) out.push_back(std::move(s));
        else {
            // простой recovery: пропустить до ';' или '}'
            synchronizeToSemicolon();
        }
    }
    if (!expect(TokenKind::RBrace, "block")) return false;
    return true;
}
```

И в `parseStmt` ДОБАВЬ в самом начале (перед return-блоком):
```cpp
    // if (cond) { ... } [else { ... }]
    if (check(TokenKind::KwIf)) {
        advance();
        if (!expect(TokenKind::LParen, "if condition")) return nullptr;
        auto cond = parseExpr();
        if (!cond) return nullptr;
        if (!expect(TokenKind::RParen, "if condition")) return nullptr;

        std::vector<ast::StmtPtr> thenB;
        if (!parseBlockStatements(thenB)) return nullptr;

        std::vector<ast::StmtPtr> elseB;
        if (match(TokenKind::KwElse)) {
            if (!parseBlockStatements(elseB)) return nullptr;
        }

        return std::make_unique<ast::IfStmt>(
            std::move(cond), std::move(thenB), std::move(elseB), startLoc);
    }
```

- [ ] **Step 6.4: Build + test**
```bash
ninja unit_tests && ctest --output-on-failure
```
Expected: 56/56.

- [ ] **Step 6.5: Commit**
```bash
git commit -am "feat(parser): if/else statement"
```

---

### Task 7: parseBlock в parseFunctionDecl

**Files:** Parser.h, Parser.cpp, test_parser.cpp.

Цель: заменить `skipBraceBody` на реальный разбор в `parseFunctionDecl`. После этого тело функции реально появляется в `FunctionDecl::body.stmts`.

- [ ] **Step 7.1: Failing test**

```cpp
TEST(Parser, FunctionBodyParsed) {
    solc::DiagnosticEngine d;
    solc::Lexer lex(
        "function f(uint256 x) public returns (uint256) {"
        "  require(x > 0);"
        "  uint256 y = x + 1;"
        "  return y;"
        "}", d);
    solc::Parser p(lex, d);
    auto f = p.parseFunctionDecl();
    ASSERT_TRUE(f.has_value());
    EXPECT_FALSE(d.hasErrors());
    ASSERT_EQ(f->body.stmts.size(), 3u);
    EXPECT_EQ(f->body.stmts[0]->kind, solc::ast::StmtKind::Require);
    EXPECT_EQ(f->body.stmts[1]->kind, solc::ast::StmtKind::VarDecl);
    EXPECT_EQ(f->body.stmts[2]->kind, solc::ast::StmtKind::Return);
}
```

- [ ] **Step 7.2: Поправь `parseFunctionDecl`**

Найди блок:
```cpp
    // тело пока пропускаем целиком — реальный разбор появится на неделе 4
    if (!check(TokenKind::LBrace)) {
        diag_.error(peek().loc, "expected '{' for function body");
        synchronizeToBraceEnd();
        return std::nullopt;
    }
    ast::Block body;
    body.loc = peek().loc;
    skipBraceBody();
```

Замени на:
```cpp
    if (!check(TokenKind::LBrace)) {
        diag_.error(peek().loc, "expected '{' for function body");
        synchronizeToBraceEnd();
        return std::nullopt;
    }
    ast::Block body;
    body.loc = peek().loc;
    if (!parseBlockStatements(body.stmts)) {
        synchronizeToBraceEnd();
        return std::nullopt;
    }
```

Также найди в конце метода:
```cpp
    fn.body       = std::move(body);
```
— должно остаться как есть (Task 2 уже поправил).

- [ ] **Step 7.3: Build + test**
```bash
ninja unit_tests && ctest --output-on-failure
```
Expected: 57/57. **Внимание:** существующий тест `Parser.FunctionBodyWithNestedBracesSkipped` теперь будет реально парсить `if (x) { return 1; } else { return 2; }` и должен пройти — все эти конструкции уже поддерживаются Task 5-6.

- [ ] **Step 7.4: Commit**
```bash
git commit -am "feat(parser): function body parsed as block of statements"
```

---

### Task 8: Полный counter.sol — финальная интеграция

**Files:** test_parser.cpp.

- [ ] **Step 8.1: Failing test** (вероятно сразу зелёный — все строительные блоки готовы)

```cpp
TEST(Parser, CounterFixtureFullBodies) {
    auto src = slurpFixtureForParser("fixtures/counter.sol");
    ASSERT_FALSE(src.empty());

    solc::DiagnosticEngine d;
    solc::Lexer lex(src, d);
    solc::Parser p(lex, d);

    auto c = p.parseContract();
    ASSERT_TRUE(c.has_value());
    EXPECT_FALSE(d.hasErrors());

    ASSERT_EQ(c->functions.size(), 2u);

    // increment: require(msg.sender == owner); uint256 newValue = value + delta; value = newValue; return newValue;
    const auto& inc = c->functions[0];
    ASSERT_EQ(inc.body.stmts.size(), 4u);
    EXPECT_EQ(inc.body.stmts[0]->kind, solc::ast::StmtKind::Require);
    EXPECT_EQ(inc.body.stmts[1]->kind, solc::ast::StmtKind::VarDecl);
    EXPECT_EQ(inc.body.stmts[2]->kind, solc::ast::StmtKind::Assign);
    EXPECT_EQ(inc.body.stmts[3]->kind, solc::ast::StmtKind::Return);

    // isOwner: return who == owner;
    const auto& io = c->functions[1];
    ASSERT_EQ(io.body.stmts.size(), 1u);
    EXPECT_EQ(io.body.stmts[0]->kind, solc::ast::StmtKind::Return);
}
```

- [ ] **Step 8.2: Build + test**
```bash
ninja unit_tests && ctest --output-on-failure
```
Expected: 58/58.

Если падает — отчитайся подробно (какой stmt оказался не на месте, какие диагностики прилетели). НЕ обходи — это сигнал что что-то в Task 3-7 неточно.

- [ ] **Step 8.3: Commit**
```bash
git commit -am "test(parser): Counter.sol bodies parsed end-to-end"
```

---

## Scope boundaries

- НЕ реализовывать `else if` синтаксис на этой неделе — текущий парсер требует `else { ... }` с явным блоком. Это в скоупе MVP (см. CLAUDE.md: `if (cond) { ... } else { ... }`).
- НЕ добавлять составные присваивания (`+=`, `-=`) — вне MVP.
- НЕ парсить дополнительные expressions: `%`, `**`, побитовые, индексация, тернарный — вне MVP.
- НЕ менять `dump()` для печати тел — это не критично, можно расширить на неделе 8.
- НЕ менять Driver / main / CLI — это неделя 8.

---

## Self-review checklist (после написания всех 8 задач)

- [ ] Каждый Stmt и Expr из MVP-подмножества `scope_coursework.md` имеет соответствующий узел AST и парсер
- [ ] Precedence корректен: `1 + 2 * 3` парсится как `(1 + (2*3))`, `a == b && c` — как `((a==b) && c)`, левая ассоциативность работает
- [ ] `else` опциональный
- [ ] `counter.sol` end-to-end: 2 функции, тела имеют правильные kind'ы statements
- [ ] Все 22 новых TEST зелёные (≈), регрессий нет

---

## Критерий готовности недели 4

- [ ] Полный `counter.sol` парсится в AST с заполненными телами
- [ ] ≥ 22 новых теста зелёные (итого ≥ 58)
- [ ] `Block::stmts` корректно заполняется
- [ ] Precedence climbing проходит все тесты ассоциативности
- [ ] Минимум 8 коммитов (по одному на task)
