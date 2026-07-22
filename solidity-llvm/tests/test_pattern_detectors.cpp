#include <gtest/gtest.h>
#include <memory>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>

#include "solc/analyzer/PatternDetectors.h"
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

TEST(TxOrigin, FlagsAuthComparison) {
    auto b = build(
        "contract W {"
        "  address owner;"
        "  function f() public { require(tx.origin == owner); }"
        "}");
    ASSERT_TRUE((bool)b);
    solc::analyzer::TxOriginDetector det;
    auto fs = det.analyze(*b.mod);
    ASSERT_EQ(fs.size(), 1u);
    EXPECT_EQ(fs[0].rule, "tx-origin");
    EXPECT_EQ(fs[0].severity, "MEDIUM");
}

TEST(TxOrigin, NoTxOriginNoFinding) {
    auto b = build(
        "contract W {"
        "  address owner;"
        "  function f() public { require(msg.sender == owner); }"
        "}");
    ASSERT_TRUE((bool)b);
    solc::analyzer::TxOriginDetector det;
    EXPECT_TRUE(det.analyze(*b.mod).empty());
}

TEST(UncheckedCall, FlagsIgnoredResult) {
    auto b = build(
        "contract C {"
        "  function f() public { msg.sender.call{value: 1}(\"\"); }"
        "}");
    ASSERT_TRUE((bool)b);
    solc::analyzer::UncheckedCallDetector det;
    auto fs = det.analyze(*b.mod);
    ASSERT_EQ(fs.size(), 1u);
    EXPECT_EQ(fs[0].rule, "unchecked-call");
    EXPECT_EQ(fs[0].severity, "LOW");
}

TEST(UncheckedCall, CheckedResultNoFinding) {
    // результат вызова используется (bool res = ...) -> не считается непроверенным
    auto b = build(
        "contract C {"
        "  function f() public {"
        "    bool ok = msg.sender.call{value: 1}(\"\");"
        "    require(ok);"
        "  }"
        "}");
    ASSERT_TRUE((bool)b);
    solc::analyzer::UncheckedCallDetector det;
    EXPECT_TRUE(det.analyze(*b.mod).empty());
}

// регресс (ревью): tx.origin, скопированный в локальную, всё равно ловится (mem2reg)
TEST(TxOrigin, ThroughLocalCopy) {
    auto b = build(
        "contract A { address owner; uint256 x;"
        "  function f() public { address o = tx.origin; require(o == owner); x = 1; } }");
    ASSERT_TRUE((bool)b);
    solc::analyzer::TxOriginDetector det;
    EXPECT_EQ(det.analyze(*b.mod).size(), 1u);
}

// регресс (ревью): результат call в неиспользуемой bool = непроверенный (mem2reg)
TEST(UncheckedCall, ResultInUnusedLocalStillUnchecked) {
    auto b = build(
        "contract A { uint256 b;"
        "  function f() public { bool ok = msg.sender.call{value: b}(\"\"); b = 0; } }");
    ASSERT_TRUE((bool)b);
    solc::analyzer::UncheckedCallDetector det;
    EXPECT_EQ(det.analyze(*b.mod).size(), 1u);
}

// регресс (ревью): tx.origin в НЕ-авторизационном сравнении (не гейтит revert)
// не должен флагаться (FP)
TEST(TxOrigin, NonAuthComparisonNoFinding) {
    auto b = build(
        "contract C { address last;"
        "  function f() public { bool same = tx.origin == msg.sender; last = msg.sender; } }");
    ASSERT_TRUE((bool)b);
    solc::analyzer::TxOriginDetector det;
    EXPECT_TRUE(det.analyze(*b.mod).empty());
}

TEST(Selfdestruct, UnprotectedFlaggedGuardedNot) {
    auto b = build(
        "contract C { address owner;"
        "  function kill() public { selfdestruct(msg.sender); }"
        "  function killAdmin() public { require(msg.sender == owner); selfdestruct(owner); } }");
    ASSERT_TRUE((bool)b);
    solc::analyzer::SelfdestructDetector det;
    auto fs = det.analyze(*b.mod);
    ASSERT_EQ(fs.size(), 1u);
    EXPECT_EQ(fs[0].function, "kill");
    EXPECT_EQ(fs[0].severity, "HIGH");
}

TEST(Delegatecall, UntrustedTargetFlaggedConstantNot) {
    auto b = build(
        "contract C {"
        "  function fwd(address impl) public { impl.delegatecall(\"\"); } }");
    ASSERT_TRUE((bool)b);
    solc::analyzer::DelegatecallDetector det;
    auto fs = det.analyze(*b.mod);
    ASSERT_EQ(fs.size(), 1u);
    EXPECT_EQ(fs[0].rule, "delegatecall");
}

// --- OracleStalenessDetector (IR-уровень: legacy CodeGen не эмитит Chainlink-маркер) ---
// хелпер: функция с Chainlink-чтением (@scsa.oracle_read + метка), опционально с block.timestamp
static llvm::Function* mkOracleFn(llvm::Module& m, const char* name, bool withTimestamp) {
    auto& ctx = m.getContext();
    auto* i256 = llvm::IntegerType::get(ctx, 256);
    auto orFn = m.getOrInsertFunction("scsa.oracle_read", llvm::FunctionType::get(i256, false));
    auto tsFn = m.getOrInsertFunction("evm.block_timestamp", llvm::FunctionType::get(i256, false));
    auto* fn = llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), false),
                                      llvm::Function::ExternalLinkage, name, m);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", fn));
    auto* cl = b.CreateCall(orFn, {}, "cl");
    cl->setMetadata("scsa.oracle.chainlink", llvm::MDNode::get(ctx, {}));
    if (withTimestamp) b.CreateCall(tsFn, {}, "ts");
    b.CreateRetVoid();
    return fn;
}

TEST(OracleStaleness, FlagsChainlinkWithoutTimestampCheck) {
    llvm::LLVMContext ctx;
    llvm::Module m("t", ctx);
    mkOracleFn(m, "getPrice", /*withTimestamp=*/false);
    solc::analyzer::OracleStalenessDetector det;
    auto fs = det.analyze(m);
    ASSERT_EQ(fs.size(), 1u);
    EXPECT_EQ(fs[0].rule, "oracle-staleness");
    EXPECT_EQ(fs[0].severity, "MEDIUM");
    EXPECT_EQ(fs[0].function, "getPrice");
}

TEST(OracleStaleness, NoFindingWhenTimestampChecked) {
    llvm::LLVMContext ctx;
    llvm::Module m("t", ctx);
    mkOracleFn(m, "getPriceChecked", /*withTimestamp=*/true);
    solc::analyzer::OracleStalenessDetector det;
    EXPECT_TRUE(det.analyze(m).empty());
}

TEST(OracleStaleness, IgnoresNonChainlinkOracleRead) {
    // spot-DEX oracle_read (без метки chainlink) — не наш класс, не флагаем
    llvm::LLVMContext ctx;
    llvm::Module m("t", ctx);
    auto* i256 = llvm::IntegerType::get(ctx, 256);
    auto orFn = m.getOrInsertFunction("scsa.oracle_read", llvm::FunctionType::get(i256, false));
    auto* fn = llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), false),
                                      llvm::Function::ExternalLinkage, "getSpot", m);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", fn));
    b.CreateCall(orFn, {}, "spot");   // без метки scsa.oracle.chainlink
    b.CreateRetVoid();
    solc::analyzer::OracleStalenessDetector det;
    EXPECT_TRUE(det.analyze(m).empty());
}

// --- SlippageDetector (IR-уровень) ---
static llvm::Function* mkSwapFn(llvm::Module& m, const char* name, bool withArgGuard) {
    auto& ctx = m.getContext();
    auto* i160 = llvm::IntegerType::get(ctx, 160);
    auto* i256 = llvm::IntegerType::get(ctx, 256);
    auto* i1 = llvm::IntegerType::get(ctx, 1);
    auto ext = m.getOrInsertFunction("scsa.external_call",
                                     llvm::FunctionType::get(i1, {i160, i256}, false));
    auto* fn = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), {i256}, false),
        llvm::Function::ExternalLinkage, name, m);   // param0 = minOut
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", fn));
    auto* call = b.CreateCall(ext, {llvm::ConstantInt::get(i160, 0), llvm::ConstantInt::get(i256, 0)}, "swap");
    call->setMetadata("scsa.swap", llvm::MDNode::get(ctx, {}));
    if (withArgGuard)   // require(out >= minOut): сравнение против аргумента
        b.CreateICmpUGE(llvm::ConstantInt::get(i256, 100), fn->getArg(0), "guard");
    b.CreateRetVoid();
    return fn;
}

TEST(Slippage, FlagsSwapWithoutArgGuard) {
    llvm::LLVMContext ctx; llvm::Module m("t", ctx);
    mkSwapFn(m, "doSwap", /*withArgGuard=*/false);
    solc::analyzer::SlippageDetector det;
    auto fs = det.analyze(m);
    ASSERT_EQ(fs.size(), 1u);
    EXPECT_EQ(fs[0].rule, "missing-slippage");
    EXPECT_EQ(fs[0].severity, "MEDIUM");
}

TEST(Slippage, NoFindingWithArgComparison) {
    llvm::LLVMContext ctx; llvm::Module m("t", ctx);
    mkSwapFn(m, "doSwapGuarded", /*withArgGuard=*/true);
    solc::analyzer::SlippageDetector det;
    EXPECT_TRUE(det.analyze(m).empty());
}

TEST(Slippage, NoSwapNoFinding) {
    llvm::LLVMContext ctx; llvm::Module m("t", ctx);
    auto* i256 = llvm::IntegerType::get(ctx, 256);
    auto* fn = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), {i256}, false),
        llvm::Function::ExternalLinkage, "noSwap", m);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", fn));
    b.CreateRetVoid();
    solc::analyzer::SlippageDetector det;
    EXPECT_TRUE(det.analyze(m).empty());
}

// permissionless harvest со свопом + min-out сравнением против аргумента, но БЕЗ msg.sender-guard
// → всё равно уязвим (caller ставит minOut=0) → флагаем (новая логика после super-vaults ханта)
TEST(Slippage, FlagsPermissionlessHarvestEvenWithArgMinOut) {
    llvm::LLVMContext ctx; llvm::Module m("t", ctx);
    auto* i256 = llvm::IntegerType::get(ctx, 256);
    auto sw = m.getOrInsertFunction("scsa.swap", llvm::FunctionType::get(i256, false));
    auto* fn = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), {i256}, false),
        llvm::Function::ExternalLinkage, "harvest", m);   // harvest-named, arg0 = caller minOut
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", fn));
    auto* out = b.CreateCall(sw, {}, "swap");             // @scsa.swap маркер (DexSwap.swap)
    b.CreateICmpUGE(out, fn->getArg(0), "guard");         // require(out >= minOut) — против аргумента
    b.CreateRetVoid();
    solc::analyzer::SlippageDetector det;
    auto fs = det.analyze(m);
    ASSERT_EQ(fs.size(), 1u);
    EXPECT_EQ(fs[0].function, "harvest");
}

// harvest со свопом, ЗАЩИЩЁН msg.sender (onlyManager keeper) → доверенный min-out → НЕ флагаем
TEST(Slippage, NoFindingGuardedHarvest) {
    llvm::LLVMContext ctx; llvm::Module m("t", ctx);
    auto* i256 = llvm::IntegerType::get(ctx, 256);
    auto* i160 = llvm::IntegerType::get(ctx, 160);
    auto sw = m.getOrInsertFunction("scsa.swap", llvm::FunctionType::get(i256, false));
    auto ms = m.getOrInsertFunction("evm.msg_sender", llvm::FunctionType::get(i160, false));
    auto* fn = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), {i160}, false),  // arg = manager
        llvm::Function::ExternalLinkage, "harvest", m);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", fn));
    auto* snd = b.CreateCall(ms, {}, "sender");
    b.CreateICmpNE(snd, fn->getArg(0), "authcheck");     // msg.sender != manager → guard
    b.CreateCall(sw, {}, "swap");
    b.CreateRetVoid();
    solc::analyzer::SlippageDetector det;
    EXPECT_TRUE(det.analyze(m).empty());
}
