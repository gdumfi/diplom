#include <gtest/gtest.h>
#include <fstream>
#include <sstream>
#include "solc/ast/AST.h"
#include "solc/lexer/Lexer.h"
#include "solc/parser/Parser.h"

static std::string slurpFixtureForParser(const std::string& relPath) {
    std::ifstream f(std::string(SOLC_TESTS_DIR) + "/" + relPath);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

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

TEST(AST, DumpEmptyContract) {
    solc::ast::ContractDecl c;
    c.name = "X";

    std::ostringstream os;
    solc::ast::dump(c, os);

    EXPECT_NE(os.str().find("contract X"), std::string::npos);
}

TEST(Parser, ParseTypeUint256) {
    solc::DiagnosticEngine d;
    solc::Lexer lex("uint256", d);
    solc::Parser p(lex, d);
    auto t = p.parseType();
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->kind, solc::ast::TypeKind::Uint256);
    EXPECT_FALSE(d.hasErrors());
}

TEST(Parser, ParseTypeBool) {
    solc::DiagnosticEngine d;
    solc::Lexer lex("bool", d);
    solc::Parser p(lex, d);
    auto t = p.parseType();
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->kind, solc::ast::TypeKind::Bool);
}

TEST(Parser, ParseTypeAddress) {
    solc::DiagnosticEngine d;
    solc::Lexer lex("address", d);
    solc::Parser p(lex, d);
    auto t = p.parseType();
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->kind, solc::ast::TypeKind::Address);
}

TEST(Parser, ParseTypeUserTypeBecomesAddress) {
    // пользовательский тип-идентификатор (контракт/интерфейс) моделируется как address
    solc::DiagnosticEngine d;
    solc::Lexer lex("ERC20", d);
    solc::Parser p(lex, d);
    auto t = p.parseType();
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->kind, solc::ast::TypeKind::Address);
    EXPECT_FALSE(d.hasErrors());
}

TEST(Parser, ParseTypeNonTypeFails) {
    // не-тип (число) на месте типа — ошибка
    solc::DiagnosticEngine d;
    solc::Lexer lex("123", d);
    solc::Parser p(lex, d);
    auto t = p.parseType();
    EXPECT_FALSE(t.has_value());
    EXPECT_TRUE(d.hasErrors());
}

TEST(Parser, ParseStateVarPublic) {
    solc::DiagnosticEngine d;
    solc::Lexer lex("uint256 public value;", d);
    solc::Parser p(lex, d);
    auto sv = p.parseStateVarDecl();
    ASSERT_TRUE(sv.has_value());
    EXPECT_EQ(sv->type.kind, solc::ast::TypeKind::Uint256);
    EXPECT_EQ(sv->name, "value");
    EXPECT_TRUE(sv->isPublic);
    EXPECT_FALSE(d.hasErrors());
}

TEST(Parser, ParseStateVarNoPublic) {
    solc::DiagnosticEngine d;
    solc::Lexer lex("address owner;", d);
    solc::Parser p(lex, d);
    auto sv = p.parseStateVarDecl();
    ASSERT_TRUE(sv.has_value());
    EXPECT_EQ(sv->type.kind, solc::ast::TypeKind::Address);
    EXPECT_EQ(sv->name, "owner");
    EXPECT_FALSE(sv->isPublic);
    EXPECT_FALSE(d.hasErrors());
}

TEST(Parser, ParseStateVarMissingName) {
    solc::DiagnosticEngine d;
    solc::Lexer lex("uint256 ;", d);
    solc::Parser p(lex, d);
    auto sv = p.parseStateVarDecl();
    EXPECT_FALSE(sv.has_value());
    EXPECT_TRUE(d.hasErrors());
}

TEST(Parser, FunctionNoParamsNoReturn) {
    solc::DiagnosticEngine d;
    solc::Lexer lex("function z() public { }", d);
    solc::Parser p(lex, d);
    auto f = p.parseFunctionDecl();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->name, "z");
    EXPECT_TRUE(f->params.empty());
    EXPECT_FALSE(f->returnType.has_value());
    EXPECT_FALSE(d.hasErrors());
}

TEST(Parser, FunctionOneParamReturnsUint) {
    solc::DiagnosticEngine d;
    solc::Lexer lex("function inc(uint256 d) public returns (uint256) { }", d);
    solc::Parser p(lex, d);
    auto f = p.parseFunctionDecl();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->name, "inc");
    ASSERT_EQ(f->params.size(), 1u);
    EXPECT_EQ(f->params[0].type.kind, solc::ast::TypeKind::Uint256);
    EXPECT_EQ(f->params[0].name, "d");
    ASSERT_TRUE(f->returnType.has_value());
    EXPECT_EQ(f->returnType->kind, solc::ast::TypeKind::Uint256);
}

TEST(Parser, FunctionTwoParams) {
    solc::DiagnosticEngine d;
    solc::Lexer lex("function multi(uint256 a, address b) public { }", d);
    solc::Parser p(lex, d);
    auto f = p.parseFunctionDecl();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->params.size(), 2u);
    EXPECT_EQ(f->params[0].type.kind, solc::ast::TypeKind::Uint256);
    EXPECT_EQ(f->params[0].name, "a");
    EXPECT_EQ(f->params[1].type.kind, solc::ast::TypeKind::Address);
    EXPECT_EQ(f->params[1].name, "b");
}

TEST(Parser, FunctionBodyWithNestedBracesSkipped) {
    // тело внутри { } может содержать произвольные токены, в т.ч. вложенные { }.
    // парсер на этой неделе должен корректно пропустить любое содержимое.
    solc::DiagnosticEngine d;
    solc::Lexer lex("function f() public { if (x) { return 1; } else { return 2; } }", d);
    solc::Parser p(lex, d);
    auto f = p.parseFunctionDecl();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->name, "f");
    EXPECT_FALSE(d.hasErrors());
}

TEST(Parser, FullContractWithVarsAndFunctions) {
    solc::DiagnosticEngine d;
    solc::Lexer lex(
        "contract Counter {"
        "  uint256 public v;"
        "  address o;"
        "  function f() public { }"
        "  function g(uint256 x) public returns (bool) { }"
        "}", d);
    solc::Parser p(lex, d);
    auto c = p.parseContract();
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->name, "Counter");
    ASSERT_EQ(c->stateVars.size(), 2u);
    EXPECT_EQ(c->stateVars[0].name, "v");
    EXPECT_TRUE(c->stateVars[0].isPublic);
    EXPECT_EQ(c->stateVars[1].name, "o");
    EXPECT_FALSE(c->stateVars[1].isPublic);
    ASSERT_EQ(c->functions.size(), 2u);
    EXPECT_EQ(c->functions[0].name, "f");
    EXPECT_EQ(c->functions[1].name, "g");
    EXPECT_FALSE(d.hasErrors());
}

TEST(Parser, ContractEmptyBody) {
    solc::DiagnosticEngine d;
    solc::Lexer lex("contract Empty { }", d);
    solc::Parser p(lex, d);
    auto c = p.parseContract();
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->name, "Empty");
    EXPECT_TRUE(c->stateVars.empty());
    EXPECT_TRUE(c->functions.empty());
}

TEST(Parser, EmptyInputProducesError) {
    solc::DiagnosticEngine d;
    solc::Lexer lex("", d);
    solc::Parser p(lex, d);

    auto c = p.parseContract();
    EXPECT_FALSE(c.has_value());
    ASSERT_TRUE(d.hasErrors());
    EXPECT_NE(d.errors()[0].message.find("contract"), std::string::npos);
}

TEST(Parser, CounterFixtureFullDeclarations) {
    auto src = slurpFixtureForParser("fixtures/counter.sol");
    ASSERT_FALSE(src.empty()) << "fixture not found";

    solc::DiagnosticEngine d;
    solc::Lexer lex(src, d);
    solc::Parser p(lex, d);

    auto c = p.parseContract();
    ASSERT_TRUE(c.has_value()) << "Counter must parse";
    EXPECT_FALSE(d.hasErrors());

    EXPECT_EQ(c->name, "Counter");

    ASSERT_EQ(c->stateVars.size(), 2u);
    EXPECT_EQ(c->stateVars[0].name, "value");
    EXPECT_EQ(c->stateVars[0].type.kind, solc::ast::TypeKind::Uint256);
    EXPECT_TRUE(c->stateVars[0].isPublic);
    EXPECT_EQ(c->stateVars[1].name, "owner");
    EXPECT_EQ(c->stateVars[1].type.kind, solc::ast::TypeKind::Address);
    EXPECT_TRUE(c->stateVars[1].isPublic);

    ASSERT_EQ(c->functions.size(), 2u);

    const auto& inc = c->functions[0];
    EXPECT_EQ(inc.name, "increment");
    ASSERT_EQ(inc.params.size(), 1u);
    EXPECT_EQ(inc.params[0].type.kind, solc::ast::TypeKind::Uint256);
    EXPECT_EQ(inc.params[0].name, "delta");
    ASSERT_TRUE(inc.returnType.has_value());
    EXPECT_EQ(inc.returnType->kind, solc::ast::TypeKind::Uint256);

    const auto& io = c->functions[1];
    EXPECT_EQ(io.name, "isOwner");
    ASSERT_EQ(io.params.size(), 1u);
    EXPECT_EQ(io.params[0].type.kind, solc::ast::TypeKind::Address);
    EXPECT_EQ(io.params[0].name, "who");
    ASSERT_TRUE(io.returnType.has_value());
    EXPECT_EQ(io.returnType->kind, solc::ast::TypeKind::Bool);
}

TEST(Parser, RecoversAfterBadStateVar) {
    // первый state var сломан (нет имени), второй корректен.
    // после recovery второй должен попасть в AST.
    solc::DiagnosticEngine d;
    solc::Lexer lex(
        "contract X {"
        "  uint256 ;"
        "  address owner;"
        "}", d);
    solc::Parser p(lex, d);

    auto c = p.parseContract();
    ASSERT_TRUE(c.has_value());
    EXPECT_TRUE(d.hasErrors());
    EXPECT_NE(d.errors()[0].message.find("identifier"), std::string::npos);
    ASSERT_EQ(c->stateVars.size(), 1u);
    EXPECT_EQ(c->stateVars[0].name, "owner");
}

TEST(Parser, FallbackFunctionParses) {
    // безымянная function() { } — fallback (старый Solidity), теперь валидна
    // и получает синтетическое имя $fallback
    solc::DiagnosticEngine d;
    solc::Lexer lex(
        "contract X {"
        "  function () public { }"
        "  uint256 public ok;"
        "}", d);
    solc::Parser p(lex, d);

    auto c = p.parseContract();
    ASSERT_TRUE(c.has_value());
    EXPECT_FALSE(d.hasErrors());
    ASSERT_EQ(c->functions.size(), 1u);
    EXPECT_EQ(c->functions[0].name, "$fallback");
    ASSERT_EQ(c->stateVars.size(), 1u);
    EXPECT_EQ(c->stateVars[0].name, "ok");
}

TEST(Parser, UnterminatedFunctionBodyProducesDiag) {
    solc::DiagnosticEngine d;
    // открыли тело функции и не закрыли — ни тела, ни контракта
    solc::Lexer lex("contract X { function f() public { ", d);
    solc::Parser p(lex, d);
    p.parseContract();
    ASSERT_TRUE(d.hasErrors());
    bool found = false;
    for (auto& e : d.errors()) {
        if (e.message.find("unterminated") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "expected an 'unterminated' diagnostic somewhere";
}

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
    // target теперь lvalue-выражение (IdentExpr), а не строка
    ASSERT_EQ(a->target->kind, solc::ast::ExprKind::Ident);
    EXPECT_EQ(static_cast<solc::ast::IdentExpr&>(*a->target).name, "y");
    EXPECT_EQ(a->value->kind, solc::ast::ExprKind::NumberLit);
}

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

    const auto& inc = c->functions[0];
    ASSERT_EQ(inc.body.stmts.size(), 4u);
    EXPECT_EQ(inc.body.stmts[0]->kind, solc::ast::StmtKind::Require);
    EXPECT_EQ(inc.body.stmts[1]->kind, solc::ast::StmtKind::VarDecl);
    EXPECT_EQ(inc.body.stmts[2]->kind, solc::ast::StmtKind::Assign);
    EXPECT_EQ(inc.body.stmts[3]->kind, solc::ast::StmtKind::Return);

    const auto& io = c->functions[1];
    ASSERT_EQ(io.body.stmts.size(), 1u);
    EXPECT_EQ(io.body.stmts[0]->kind, solc::ast::StmtKind::Return);
}

TEST(Parser, LowLevelCallStatement) {
    using namespace solc::ast;
    const char* src =
        "contract C {"
        "  function f() public {"
        "    msg.sender.call{value: 1}(\"\");"
        "  }"
        "}";
    solc::DiagnosticEngine d;
    solc::Lexer lex(src, d);
    solc::Parser p(lex, d);
    auto c = p.parseContract();
    ASSERT_TRUE(c.has_value());
    EXPECT_FALSE(d.hasErrors());

    ASSERT_EQ(c->functions.size(), 1u);
    const auto& body = c->functions[0].body.stmts;
    ASSERT_EQ(body.size(), 1u);
    ASSERT_EQ(body[0]->kind, StmtKind::Expr);

    const auto& es = static_cast<const ExprStmt&>(*body[0]);
    ASSERT_EQ(es.expr->kind, ExprKind::LowLevelCall);
    const auto& lc = static_cast<const LowLevelCallExpr&>(*es.expr);
    // target = msg.sender (member-access), value = литерал 1
    EXPECT_EQ(lc.target->kind, ExprKind::Member);
    EXPECT_EQ(lc.value->kind, ExprKind::NumberLit);
}

TEST(Parser, ReentrancyFixtureParses) {
    auto src = slurpFixtureForParser("fixtures/reentrancy.sol");
    ASSERT_FALSE(src.empty());
    solc::DiagnosticEngine d;
    solc::Lexer lex(src, d);
    solc::Parser p(lex, d);
    auto c = p.parseContract();
    ASSERT_TRUE(c.has_value());
    EXPECT_FALSE(d.hasErrors());
    ASSERT_EQ(c->functions.size(), 2u);

    // withdraw: require; external-call-stmt; assign
    const auto& wd = c->functions[1];
    ASSERT_EQ(wd.body.stmts.size(), 3u);
    EXPECT_EQ(wd.body.stmts[0]->kind, solc::ast::StmtKind::Require);
    EXPECT_EQ(wd.body.stmts[1]->kind, solc::ast::StmtKind::Expr);
    EXPECT_EQ(wd.body.stmts[2]->kind, solc::ast::StmtKind::Assign);
}

TEST(Parser, MappingStateVarAndIndex) {
    using namespace solc::ast;
    const char* src =
        "contract C {"
        "  mapping(address => uint256) balances;"
        "  function f() public { balances[msg.sender] = 1; }"
        "}";
    solc::DiagnosticEngine d;
    solc::Lexer lex(src, d);
    solc::Parser p(lex, d);
    auto c = p.parseContract();
    ASSERT_TRUE(c.has_value());
    EXPECT_FALSE(d.hasErrors());
    ASSERT_EQ(c->stateVars.size(), 1u);
    EXPECT_EQ(c->stateVars[0].type.kind, TypeKind::Mapping);
    EXPECT_EQ(c->stateVars[0].type.mapKey, TypeKind::Address);
    EXPECT_EQ(c->stateVars[0].type.mapValue, TypeKind::Uint256);
    // тело: присваивание, цель — IndexExpr
    const auto& body = c->functions[0].body.stmts;
    ASSERT_EQ(body.size(), 1u);
    ASSERT_EQ(body[0]->kind, StmtKind::Assign);
    const auto& a = static_cast<const AssignStmt&>(*body[0]);
    EXPECT_EQ(a.target->kind, ExprKind::Index);
}

// регресс (ревью-фаззер): патологически глубокая вложенность не должна ронять
// процесс — парсер обязан gracefully выдать ошибку, а не переполнить стек
TEST(Parser, DeepNestingDoesNotCrash) {
    std::string src = "contract C{function f() public returns(uint256){return ";
    for (int i = 0; i < 5000; ++i) src += "(";
    src += "1";
    for (int i = 0; i < 5000; ++i) src += ")";
    src += ";}}";
    solc::DiagnosticEngine d;
    solc::Lexer lex(src, d);
    solc::Parser p(lex, d);
    auto c = p.parseContract();   // не должно упасть
    EXPECT_TRUE(d.hasErrors());   // должна быть диагностика глубины
}

TEST(Parser, SourceUnitMultipleContracts) {
    solc::DiagnosticEngine d;
    solc::Lexer lex("pragma solidity ^0.4.0; contract A { } contract B { }", d);
    solc::Parser p(lex, d);
    auto cs = p.parseSourceUnit();
    EXPECT_FALSE(d.hasErrors());
    ASSERT_EQ(cs.size(), 2u);
    EXPECT_EQ(cs[0].name, "A");
    EXPECT_EQ(cs[1].name, "B");
}

TEST(Parser, InheritanceClauseParses) {
    solc::DiagnosticEngine d;
    solc::Lexer lex("contract Ownable { } contract Wallet is Ownable { uint256 c; }", d);
    solc::Parser p(lex, d);
    auto cs = p.parseSourceUnit();
    EXPECT_FALSE(d.hasErrors());
    ASSERT_EQ(cs.size(), 2u);
    EXPECT_EQ(cs[1].name, "Wallet");
    // имена баз сохранены (для пост-прохода слияния)
    ASSERT_EQ(cs[1].baseContracts.size(), 1u);
    EXPECT_EQ(cs[1].baseContracts[0], "Ownable");
    EXPECT_TRUE(cs[0].baseContracts.empty());
}

TEST(Parser, MultipleBasesSaved) {
    solc::DiagnosticEngine d;
    solc::Lexer lex("contract A {} contract B {} contract C is A, B { uint256 x; }", d);
    solc::Parser p(lex, d);
    auto cs = p.parseSourceUnit();
    EXPECT_FALSE(d.hasErrors());
    ASSERT_EQ(cs.size(), 3u);
    ASSERT_EQ(cs[2].baseContracts.size(), 2u);
    EXPECT_EQ(cs[2].baseContracts[0], "A");
    EXPECT_EQ(cs[2].baseContracts[1], "B");
}
