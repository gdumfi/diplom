#include <gtest/gtest.h>
#include <fstream>
#include <sstream>
#include "solc/sema/Symbol.h"
#include "solc/lexer/Lexer.h"
#include "solc/parser/Parser.h"
#include "solc/sema/Sema.h"

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
    Symbol a{SymbolKind::Local, "y",
             solc::ast::Type{solc::ast::TypeKind::Bool, {}}, -1, {}};
    Symbol b = a;
    EXPECT_TRUE(st.declare(a));
    EXPECT_FALSE(st.declare(b));
}

TEST(SymbolTable, ShadowingInInnerScope) {
    using namespace solc::sema;
    SymbolTable st;
    st.enterScope();
    st.declare({SymbolKind::StateVar, "z",
                solc::ast::Type{solc::ast::TypeKind::Uint256, {}}, 0, {}});
    st.enterScope();
    st.declare({SymbolKind::Local, "z",
                solc::ast::Type{solc::ast::TypeKind::Bool, {}}, -1, {}});
    Symbol* inner = st.lookup("z");
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->kind, solc::sema::SymbolKind::Local);
    st.leaveScope();
    Symbol* outer = st.lookup("z");
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->kind, solc::sema::SymbolKind::StateVar);
}

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

// helper: анализ исходника, возвращает (успех, есть_ли_ошибки)
static bool semaOk(const char* src) {
    solc::DiagnosticEngine d;
    solc::Lexer lex(src, d);
    solc::Parser p(lex, d);
    auto c = p.parseContract();
    if (!c) return false;
    solc::sema::SemanticAnalyzer sa(d);
    return sa.analyze(*c) && !d.hasErrors();
}

TEST(Sema, LowLevelCallOnAddressOk) {
    EXPECT_TRUE(semaOk(
        "contract C {"
        "  function f() public { msg.sender.call{value: 1}(\"\"); }"
        "}"));
}

TEST(Sema, LowLevelCallValueMustBeUint) {
    // value: true — bool вместо uint256 → ошибка типа
    EXPECT_FALSE(semaOk(
        "contract C {"
        "  function f() public { msg.sender.call{value: true}(\"\"); }"
        "}"));
}

TEST(Sema, ReentrancyFixtureAnalyzes) {
    std::ifstream f(std::string(SOLC_TESTS_DIR) + "/fixtures/reentrancy.sol");
    std::stringstream ss; ss << f.rdbuf();
    std::string src = ss.str();
    ASSERT_FALSE(src.empty());
    EXPECT_TRUE(semaOk(src.c_str()));
}

TEST(Sema, MappingReadWriteOk) {
    EXPECT_TRUE(semaOk(
        "contract C {"
        "  mapping(address => uint256) b;"
        "  function f() public returns (uint256) {"
        "    b[msg.sender] = b[msg.sender] + 1;"
        "    return b[msg.sender];"
        "  }"
        "}"));
}

TEST(Sema, MappingWrongKeyTypeFails) {
    // ключ uint256 вместо address
    EXPECT_FALSE(semaOk(
        "contract C {"
        "  mapping(address => uint256) b;"
        "  function f(uint256 k) public { b[k] = 1; }"
        "}"));
}

TEST(Sema, MappingLocalRejected) {
    EXPECT_FALSE(semaOk(
        "contract C {"
        "  function f() public { mapping(address => uint256) m = m; }"
        "}"));
}
