#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <sstream>

#include "solc/analyzer/AccessControlDetector.h"
#include "solc/analyzer/AnalysisPrep.h"
#include "solc/codegen/CodeGen.h"
#include "solc/lexer/Lexer.h"
#include "solc/parser/Parser.h"
#include "solc/sema/Sema.h"

struct Built {
    std::unique_ptr<solc::codegen::CodeGen> cg;
    std::unique_ptr<llvm::Module>           mod;
    operator bool() const { return mod != nullptr; }
};
static Built build(const char* src) {
    solc::DiagnosticEngine d;
    solc::Lexer lex(src, d);
    solc::Parser p(lex, d);
    auto c = p.parseContract();
    if (!c) return {};
    solc::sema::SemanticAnalyzer sa(d);
    if (!sa.analyze(*c)) return {};
    auto cg = std::make_unique<solc::codegen::CodeGen>(d);
    auto mod = cg->generate(*c);
    if (mod) solc::analyzer::promoteLocals(*mod);
    return {std::move(cg), std::move(mod)};
}
static std::string slurp(const std::string& rel) {
    std::ifstream f(std::string(SOLC_TESTS_DIR) + "/" + rel);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

TEST(AccessControl, FlagsUncheckedPrivilegedWrites) {
    auto src = slurp("fixtures/access_control.sol");
    ASSERT_FALSE(src.empty());
    auto b = build(src.c_str());
    ASSERT_TRUE((bool)b);
    solc::analyzer::AccessControlDetector det;
    auto fs = det.analyze(*b.mod);

    // setConfig (config) и takeOwner (owner) — обе без проверки
    ASSERT_EQ(fs.size(), 2u);
    for (const auto& f : fs) {
        EXPECT_EQ(f.rule, "access-control");
        EXPECT_EQ(f.severity, "HIGH");
        EXPECT_NE(f.function, "setConfigAdmin");   // проверенная — не флагуется
    }
}

// контракт без понятия owner (нет сравнения msg.sender со storage) — не гадаем, 0 находок
TEST(AccessControl, NoOwnerConceptNoFindings) {
    auto b = build(
        "contract C {"
        "  uint256 x;"
        "  function set(uint256 v) public { x = v; }"
        "}");
    ASSERT_TRUE((bool)b);
    solc::analyzer::AccessControlDetector det;
    EXPECT_TRUE(det.analyze(*b.mod).empty());
}

// каждая привилегированная запись под проверкой — 0 находок (нет ложных)
TEST(AccessControl, AllCheckedNoFindings) {
    auto b = build(
        "contract C {"
        "  address owner;"
        "  uint256 cfg;"
        "  function a(uint256 v) public { require(msg.sender == owner); cfg = v; }"
        "  function b(address o) public { require(msg.sender == owner); owner = o; }"
        "}");
    ASSERT_TRUE((bool)b);
    solc::analyzer::AccessControlDetector det;
    EXPECT_TRUE(det.analyze(*b.mod).empty());
}

// reentrancy-фикстура не должна давать ложных access-control находок
TEST(AccessControl, ReentrancyFixtureNoAccessFindings) {
    auto src = slurp("fixtures/reentrancy.sol");
    ASSERT_FALSE(src.empty());
    auto b = build(src.c_str());
    ASSERT_TRUE((bool)b);
    solc::analyzer::AccessControlDetector det;
    EXPECT_TRUE(det.analyze(*b.mod).empty());
}

// onlyOwner-модификатор: require встраивается в тело функции -> НЕ ложное срабатывание
TEST(AccessControl, OnlyOwnerModifierNoFalsePositive) {
    auto b = build(
        "contract Bank {"
        "  address owner;"
        "  uint256 config;"
        "  modifier onlyOwner() { require(msg.sender == owner); _; }"
        "  function setConfig(uint256 c) public onlyOwner { config = c; }"
        "  function setOpen(uint256 c) public { config = c; }"
        "}");
    ASSERT_TRUE((bool)b);
    solc::analyzer::AccessControlDetector det;
    auto fs = det.analyze(*b.mod);
    // защищённая модификатором setConfig не флагуется; открытая setOpen — да
    ASSERT_EQ(fs.size(), 1u);
    EXPECT_EQ(fs[0].function, "setOpen");
}

// регресс (ревью): FP-фиксы — конструктор, mapping-запись, adminPay не флагаются;
// FN-фикс — фиктивная проверка (не гейтит revert) флагуется
TEST(AccessControl, ReviewFixesNoFalsePositivesAndFakeCheckCaught) {
    auto b = build(
        "contract Bank {"
        "  address owner;"
        "  mapping(address => uint256) balances;"
        "  function Bank() public { owner = msg.sender; }"
        "  function adminPay(address u, uint256 a) public { require(msg.sender == owner); balances[u] = a; }"
        "  function deposit() public { balances[msg.sender] = balances[msg.sender] + msg.value; }"
        "  function takeOwner(address o) public { owner = o; }"
        "}");
    ASSERT_TRUE((bool)b);
    solc::analyzer::AccessControlDetector det;
    auto fs = det.analyze(*b.mod);
    ASSERT_EQ(fs.size(), 1u);              // только takeOwner
    EXPECT_EQ(fs[0].function, "takeOwner");
}

TEST(AccessControl, FakeCheckNotGatingRevertIsFlagged) {
    auto b = build(
        "contract B {"
        "  address owner; uint256 config;"
        "  function admin(uint256 c) public { require(msg.sender == owner); config = c; }"
        "  function fake(uint256 c) public { bool ok = msg.sender == owner; config = c; }"
        "}");
    ASSERT_TRUE((bool)b);
    solc::analyzer::AccessControlDetector det;
    auto fs = det.analyze(*b.mod);
    ASSERT_EQ(fs.size(), 1u);
    EXPECT_EQ(fs[0].function, "fake");
}

// регресс (ревью): owner-проверка внутри require(... && msg.sender==owner) должна
// распознаваться (codegen && = and i1) — иначе backdoor-функция пропускается (FN)
TEST(AccessControl, OwnerCheckInsideAndRecognized) {
    auto b = build(
        "contract C { address owner; uint256 config;"
        "  function admin(uint256 x) public { require(msg.sender == owner && x > 0); config = x; }"
        "  function other(uint256 x) public { config = x; } }");
    ASSERT_TRUE((bool)b);
    solc::analyzer::AccessControlDetector det;
    auto fs = det.analyze(*b.mod);
    ASSERT_EQ(fs.size(), 1u);
    EXPECT_EQ(fs[0].function, "other");   // admin распознан как защищённый
}

// регресс (ревью): per-token auth require(msg.sender==owners[x]) — НЕ глобальный
// admin; config не должен становиться привилегированным, other не флагуется
TEST(AccessControl, PerTokenMappingAuthNoFalsePositive) {
    auto b = build(
        "contract C { mapping(uint256 => address) owners; uint256 config;"
        "  function setConfig(uint256 x) public { require(msg.sender == owners[x]); config = x; }"
        "  function other(uint256 x) public { config = x; } }");
    ASSERT_TRUE((bool)b);
    solc::analyzer::AccessControlDetector det;
    EXPECT_TRUE(det.analyze(*b.mod).empty());
}

// отложенный баг (ревью): unprotected withdrawal — вывод средств произвольному
// адресу без проверки (owner-концепт есть) должен флагаться HIGH
TEST(AccessControl, UnprotectedWithdrawalFlagged) {
    auto b = build(
        "contract Vault { address owner; uint256 fee;"
        "  function setFee(uint256 f) public { require(msg.sender == owner); fee = f; }"
        "  function withdrawAll(address to, uint256 amount) public { to.call{value: amount}(\"\"); } }");
    ASSERT_TRUE((bool)b);
    solc::analyzer::AccessControlDetector det;
    auto fs = det.analyze(*b.mod);
    bool w = false;
    for (const auto& f : fs) if (f.function == "withdrawAll" && f.severity == "HIGH") w = true;
    EXPECT_TRUE(w);
}

// вывод на msg.sender (свои средства) — НЕ unprotected withdrawal
TEST(AccessControl, SelfWithdrawNotFlagged) {
    auto b = build(
        "contract Vault { address owner; uint256 fee;"
        "  function setFee(uint256 f) public { require(msg.sender == owner); fee = f; }"
        "  function withdraw(uint256 amount) public { msg.sender.call{value: amount}(\"\"); } }");
    ASSERT_TRUE((bool)b);
    solc::analyzer::AccessControlDetector det;
    for (const auto& f : det.analyze(*b.mod))
        EXPECT_NE(f.function, "withdraw");   // отправка себе — легитимно
}
