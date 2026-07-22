#include <gtest/gtest.h>
#include <algorithm>
#include <memory>

#include "solc/analyzer/AccessControlDetector.h"
#include "solc/analyzer/AnalysisPrep.h"
#include "solc/codegen/CodeGen.h"
#include "solc/ast/AST.h"
#include "solc/lexer/Lexer.h"
#include "solc/parser/Parser.h"
#include "solc/sema/Inheritance.h"
#include "solc/sema/Sema.h"

using solc::ast::ContractDecl;

// парсит source unit и сливает наследование
static std::vector<ContractDecl> parseMerge(const char* src) {
    solc::DiagnosticEngine d;
    solc::Lexer lex(src, d);
    solc::Parser p(lex, d);
    auto cs = p.parseSourceUnit();
    return solc::sema::mergeInheritance(std::move(cs), d);
}
static const ContractDecl* find(const std::vector<ContractDecl>& cs, const std::string& n) {
    for (const auto& c : cs) if (c.name == n) return &c;
    return nullptr;
}
static bool hasStateVar(const ContractDecl& c, const std::string& n) {
    return std::any_of(c.stateVars.begin(), c.stateVars.end(),
                       [&](const auto& sv){ return sv.name == n; });
}
static bool hasModifier(const ContractDecl& c, const std::string& n) {
    return std::any_of(c.modifiers.begin(), c.modifiers.end(),
                       [&](const auto& md){ return md.name == n; });
}

// ИНВАРИАНТ 0-РЕГРЕССА: файл без наследования → выход тождествен входу
TEST(Inheritance, NoInheritanceIdentity) {
    auto cs = parseMerge("contract A { uint256 x; uint256 y; } contract B { bool f; }");
    ASSERT_EQ(cs.size(), 2u);
    const auto* a = find(cs, "A");
    ASSERT_NE(a, nullptr);
    ASSERT_EQ(a->stateVars.size(), 2u);
    EXPECT_EQ(a->stateVars[0].name, "x");   // порядок сохранён → те же слоты в sema
    EXPECT_EQ(a->stateVars[1].name, "y");
}

// OZ Ownable: производный контракт получает owner (state var) и onlyOwner (модификатор)
TEST(Inheritance, OwnableMergedIntoDerived) {
    auto cs = parseMerge(
        "contract Ownable { address owner; modifier onlyOwner { require(msg.sender==owner); _; } }"
        "contract Vault is Ownable { uint256 config;"
        "  function setX(uint256 v) public onlyOwner { config = v; } }");
    const auto* v = find(cs, "Vault");
    ASSERT_NE(v, nullptr);
    EXPECT_TRUE(hasStateVar(*v, "owner"));    // унаследован
    EXPECT_TRUE(hasStateVar(*v, "config"));
    EXPECT_TRUE(hasModifier(*v, "onlyOwner")); // унаследованный модификатор доступен для инлайна
    // owner идёт ПЕРЕД config (base-first → слот owner=0, config=1)
    EXPECT_EQ(v->stateVars[0].name, "owner");
}

// многоуровневая цепочка A -> B -> C: flat C содержит все унаследованные state-vars
TEST(Inheritance, MultilevelChain) {
    auto cs = parseMerge(
        "contract A { uint256 a; }"
        "contract B is A { uint256 b; }"
        "contract C is B { uint256 c; }");
    const auto* c = find(cs, "C");
    ASSERT_NE(c, nullptr);
    EXPECT_TRUE(hasStateVar(*c, "a"));
    EXPECT_TRUE(hasStateVar(*c, "b"));
    EXPECT_TRUE(hasStateVar(*c, "c"));
    // base-first: a, b, c
    ASSERT_EQ(c->stateVars.size(), 3u);
    EXPECT_EQ(c->stateVars[0].name, "a");
    EXPECT_EQ(c->stateVars[2].name, "c");
}

// коллизия имени state-var (owner в базе и в derived) → в flat ОДИН раз (иначе sema падает)
TEST(Inheritance, ShadowedStateVarDeduped) {
    auto cs = parseMerge(
        "contract Base { address owner; }"
        "contract Derived is Base { address owner; uint256 x; }");
    const auto* d = find(cs, "Derived");
    ASSERT_NE(d, nullptr);
    int owners = 0;
    for (const auto& sv : d->stateVars) if (sv.name == "owner") owners++;
    EXPECT_EQ(owners, 1);   // дедуп по имени
}

// база вне файла (симуляция import) — не падаем, контракт остаётся валиден
TEST(Inheritance, ImportedBaseNoCrash) {
    auto cs = parseMerge("contract X is ExternalBase { uint256 x; }");
    const auto* x = find(cs, "X");
    ASSERT_NE(x, nullptr);
    EXPECT_TRUE(hasStateVar(*x, "x"));   // собственные члены на месте
}

// цикл наследования A is B, B is A — visited-set гасит рекурсию, не падаем
TEST(Inheritance, CycleNoCrash) {
    auto cs = parseMerge("contract A is B { uint256 a; } contract B is A { uint256 b; }");
    EXPECT_EQ(cs.size(), 2u);   // не зациклились, оба контракта на месте
}

// ИНТЕГРАЦИЯ (главная ценность): слияние ВКЛЮЧАЕТ детекцию access-control, ранее
// невидимую. onlyOwner из базы инлайнится → незащищённая функция даёт HIGH,
// защищённая (унаследованным модификатором) — нет.
TEST(Inheritance, MergeEnablesAccessControlDetection) {
    auto cs = parseMerge(
        "contract Ownable { address owner; modifier onlyOwner { require(msg.sender==owner); _; } }"
        "contract Vault is Ownable { uint256 config;"
        "  function setConfigSafe(uint256 c) public onlyOwner { config = c; }"
        "  function setConfigUnsafe(uint256 c) public { config = c; } }");
    ContractDecl* vault = nullptr;
    for (auto& c : cs) if (c.name == "Vault") vault = &c;
    ASSERT_NE(vault, nullptr);

    solc::DiagnosticEngine d;
    solc::sema::SemanticAnalyzer sa(d);
    ASSERT_TRUE(sa.analyze(*vault));
    solc::codegen::CodeGen cg(d);
    auto mod = cg.generate(*vault);
    ASSERT_NE(mod, nullptr);
    solc::analyzer::promoteLocals(*mod);

    solc::analyzer::AccessControlDetector det;
    bool unsafeFlagged = false, safeFlagged = false;
    for (const auto& f : det.analyze(*mod)) {
        if (f.rule == "access-control" && f.function == "setConfigUnsafe") unsafeFlagged = true;
        if (f.rule == "access-control" && f.function == "setConfigSafe")   safeFlagged = true;
    }
    EXPECT_TRUE(unsafeFlagged);   // незащищённая — HIGH (раньше пропускалась)
    EXPECT_FALSE(safeFlagged);    // защищённая унаследованным onlyOwner — чисто
}
