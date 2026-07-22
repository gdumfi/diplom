#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <sstream>

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h>

#include "solc/ast/AST.h"
#include "solc/codegen/CodeGen.h"
#include "solc/lexer/Lexer.h"
#include "solc/parser/Parser.h"
#include "solc/sema/Sema.h"

// держим CodeGen живым после выхода из helper'а — Module не владеет LLVMContext,
// а CodeGen — владеет, поэтому без этого получим dangling pointer на context
struct Compiled {
    std::unique_ptr<solc::codegen::CodeGen> cg;
    std::unique_ptr<llvm::Module>           mod;
    operator bool() const { return mod != nullptr; }
};

static Compiled compileSrc(const char* src, solc::DiagnosticEngine& d) {
    solc::Lexer lex(src, d);
    solc::Parser p(lex, d);
    auto c = p.parseContract();
    if (!c) return {};
    solc::sema::SemanticAnalyzer sa(d);
    if (!sa.analyze(*c)) return {};
    auto cg = std::make_unique<solc::codegen::CodeGen>(d);
    auto mod = cg->generate(*c);
    return {std::move(cg), std::move(mod)};
}

TEST(CodeGen, EmitsEmptyModule) {
    solc::ast::ContractDecl c;
    c.name = "X";

    solc::DiagnosticEngine d;
    solc::codegen::CodeGen cg(d);
    auto mod = cg.generate(c);
    ASSERT_NE(mod, nullptr);
    EXPECT_FALSE(llvm::verifyModule(*mod, nullptr));
}

// storage больше не через sload/sstore — вместо них load/store в addrspace(1) (AD-3)
TEST(CodeGen, NoLegacySloadSstoreFunctions) {
    solc::ast::ContractDecl c;
    c.name = "X";

    solc::DiagnosticEngine d;
    solc::codegen::CodeGen cg(d);
    auto mod = cg.generate(c);
    ASSERT_NE(mod, nullptr);
    EXPECT_EQ(mod->getFunction("evm.sload"),  nullptr);
    EXPECT_EQ(mod->getFunction("evm.sstore"), nullptr);
}

TEST(CodeGen, DeclaresExternalCallIntrinsic) {
    solc::ast::ContractDecl c;
    c.name = "X";

    solc::DiagnosticEngine d;
    solc::codegen::CodeGen cg(d);
    auto mod = cg.generate(c);
    ASSERT_NE(mod, nullptr);

    auto* ext = mod->getFunction("scsa.external_call");
    ASSERT_NE(ext, nullptr);
    EXPECT_TRUE(ext->getReturnType()->isIntegerTy(1));
    ASSERT_EQ(ext->arg_size(), 2u);
    EXPECT_TRUE(ext->getArg(0)->getType()->isIntegerTy(160));  // target
    EXPECT_TRUE(ext->getArg(1)->getType()->isIntegerTy(256));  // value
}

// datalayout объявляет указатели EVM-адрес-пространств 256-битными
TEST(CodeGen, StorageAddressSpaceInDataLayout) {
    solc::ast::ContractDecl c;
    c.name = "X";
    solc::DiagnosticEngine d;
    solc::codegen::CodeGen cg(d);
    auto mod = cg.generate(c);
    ASSERT_NE(mod, nullptr);
    EXPECT_NE(mod->getDataLayoutStr().find("p1:256:256"), std::string::npos);
}

TEST(CodeGen, FunctionPrototypesEmitted) {
    solc::DiagnosticEngine d;
    auto c = compileSrc(
        "contract C {"
        "  function f(uint256 x) public returns (uint256) { return x; }"
        "}", d);
    ASSERT_TRUE(c);
    auto* f = c.mod->getFunction("f");
    ASSERT_NE(f, nullptr);
    EXPECT_TRUE(f->getReturnType()->isIntegerTy(256));
    EXPECT_EQ(f->arg_size(), 1u);
    EXPECT_TRUE(f->getArg(0)->getType()->isIntegerTy(256));
}

TEST(CodeGen, ReturnsParamThroughLoad) {
    solc::DiagnosticEngine d;
    auto c = compileSrc(
        "contract C {"
        "  function f(uint256 x) public returns (uint256) { return x; }"
        "}", d);
    ASSERT_TRUE(c);
    auto* f = c.mod->getFunction("f");
    ASSERT_NE(f, nullptr);

    int loads = 0, rets = 0;
    for (auto& bb : *f) for (auto& inst : bb) {
        if (llvm::isa<llvm::LoadInst>(&inst))   loads++;
        if (llvm::isa<llvm::ReturnInst>(&inst)) rets++;
    }
    EXPECT_GE(loads, 1);
    EXPECT_EQ(rets, 1);
}

// счётчики load/store в storage-адресном пространстве (addrspace 1)
static int countStorageLoads(llvm::Function* f) {
    int n = 0;
    for (auto& bb : *f) for (auto& inst : bb)
        if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(&inst))
            if (ld->getPointerAddressSpace() == 1) n++;
    return n;
}
static int countStorageStores(llvm::Function* f) {
    int n = 0;
    for (auto& bb : *f) for (auto& inst : bb)
        if (auto* st = llvm::dyn_cast<llvm::StoreInst>(&inst))
            if (st->getPointerAddressSpace() == 1) n++;
    return n;
}

TEST(CodeGen, StateVarReadEmitsAddrSpaceLoad) {
    solc::DiagnosticEngine d;
    auto c = compileSrc(
        "contract C {"
        "  uint256 value;"
        "  function f() public returns (uint256) { return value; }"
        "}", d);
    ASSERT_TRUE(c);
    auto* f = c.mod->getFunction("f");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(countStorageLoads(f), 1);
}

TEST(CodeGen, StateVarWriteEmitsAddrSpaceStore) {
    solc::DiagnosticEngine d;
    auto c = compileSrc(
        "contract C {"
        "  uint256 value;"
        "  function f(uint256 x) public { value = x; }"
        "}", d);
    ASSERT_TRUE(c);
    auto* f = c.mod->getFunction("f");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(countStorageStores(f), 1);
    // запись в storage помечена метаданным !scsa.storage со слотом
    bool tagged = false;
    for (auto& bb : *f) for (auto& inst : bb)
        if (auto* st = llvm::dyn_cast<llvm::StoreInst>(&inst))
            if (st->getPointerAddressSpace() == 1 && st->getMetadata("scsa.storage"))
                tagged = true;
    EXPECT_TRUE(tagged);
}

TEST(CodeGen, AddProducesIRAdd) {
    solc::DiagnosticEngine d;
    auto c = compileSrc(
        "contract C {"
        "  function f(uint256 a, uint256 b) public returns (uint256) { return a + b; }"
        "}", d);
    ASSERT_TRUE(c);
    auto* f = c.mod->getFunction("f");
    ASSERT_NE(f, nullptr);
    int adds = 0;
    for (auto& bb : *f) for (auto& inst : bb) {
        if (inst.getOpcode() == llvm::Instruction::Add) adds++;
    }
    EXPECT_GE(adds, 1);
}

TEST(CodeGen, EqEmitsICmpEQ) {
    solc::DiagnosticEngine d;
    auto c = compileSrc(
        "contract C {"
        "  function f(uint256 a, uint256 b) public returns (bool) { return a == b; }"
        "}", d);
    ASSERT_TRUE(c);
    auto* f = c.mod->getFunction("f");
    ASSERT_NE(f, nullptr);
    int eqs = 0;
    for (auto& bb : *f) for (auto& inst : bb) {
        if (auto* ic = llvm::dyn_cast<llvm::ICmpInst>(&inst)) {
            if (ic->getPredicate() == llvm::CmpInst::ICMP_EQ) eqs++;
        }
    }
    EXPECT_EQ(eqs, 1);
}

TEST(CodeGen, NotEmitsXorWithOne) {
    solc::DiagnosticEngine d;
    auto c = compileSrc(
        "contract C {"
        "  function f(bool b) public returns (bool) { return !b; }"
        "}", d);
    ASSERT_TRUE(c);
    auto* f = c.mod->getFunction("f");
    ASSERT_NE(f, nullptr);
    int xors = 0;
    for (auto& bb : *f) for (auto& inst : bb) {
        if (inst.getOpcode() == llvm::Instruction::Xor) xors++;
    }
    EXPECT_GE(xors, 1);
}

TEST(CodeGen, VarDeclCreatesAllocaAndStore) {
    solc::DiagnosticEngine d;
    auto c = compileSrc(
        "contract C {"
        "  function f(uint256 a, uint256 b) public returns (uint256) {"
        "    uint256 y = a + b;"
        "    return y;"
        "  }"
        "}", d);
    ASSERT_TRUE(c);
    auto* f = c.mod->getFunction("f");
    ASSERT_NE(f, nullptr);
    int allocas = 0, stores = 0;
    for (auto& bb : *f) for (auto& inst : bb) {
        if (llvm::isa<llvm::AllocaInst>(&inst)) allocas++;
        if (llvm::isa<llvm::StoreInst>(&inst))  stores++;
    }
    EXPECT_GE(allocas, 3);
    EXPECT_GE(stores, 3);
}

TEST(CodeGen, IfElseEmitsBranchAndBlocks) {
    solc::DiagnosticEngine d;
    auto c = compileSrc(
        "contract C {"
        "  function f(uint256 x) public returns (uint256) {"
        "    if (x == 0) { return 1; } else { return 2; }"
        "  }"
        "}", d);
    ASSERT_TRUE(c);
    auto* f = c.mod->getFunction("f");
    ASSERT_NE(f, nullptr);

    int blocks = 0, branches = 0;
    for (auto& bb : *f) {
        blocks++;
        for (auto& inst : bb) {
            if (llvm::isa<llvm::BranchInst>(&inst)) branches++;
        }
    }
    EXPECT_GE(blocks, 3);
    EXPECT_GE(branches, 1);
}

TEST(CodeGen, ParameterAllocaInEntry) {
    solc::DiagnosticEngine d;
    auto c = compileSrc(
        "contract C {"
        "  function f(uint256 x) public returns (uint256) { return x; }"
        "}", d);
    ASSERT_TRUE(c);
    auto* f = c.mod->getFunction("f");
    ASSERT_NE(f, nullptr);
    ASSERT_FALSE(f->empty());
    auto& entry = f->getEntryBlock();
    int allocaCount = 0;
    for (auto& inst : entry) {
        if (llvm::isa<llvm::AllocaInst>(&inst)) allocaCount++;
    }
    EXPECT_EQ(allocaCount, 1);
}

TEST(CodeGen, DeclaresRevertAndMsgIntrinsics) {
    solc::ast::ContractDecl c;
    c.name = "X";

    solc::DiagnosticEngine d;
    solc::codegen::CodeGen cg(d);
    auto mod = cg.generate(c);
    ASSERT_NE(mod, nullptr);

    auto* rev    = mod->getFunction("evm.revert");
    auto* sender = mod->getFunction("evm.msg_sender");
    auto* value  = mod->getFunction("evm.msg_value");
    ASSERT_NE(rev,    nullptr);
    ASSERT_NE(sender, nullptr);
    ASSERT_NE(value,  nullptr);

    EXPECT_TRUE(rev->getReturnType()->isVoidTy());
    EXPECT_EQ(rev->arg_size(), 0u);
    EXPECT_TRUE(rev->doesNotReturn());

    EXPECT_TRUE(sender->getReturnType()->isIntegerTy(160));
    EXPECT_TRUE(value->getReturnType()->isIntegerTy(256));
}

TEST(CodeGen, RequireEmitsRevertBranch) {
    solc::DiagnosticEngine d;
    auto c = compileSrc(
        "contract C {"
        "  function f(uint256 x) public returns (uint256) {"
        "    require(x == 0);"
        "    return x;"
        "  }"
        "}", d);
    ASSERT_TRUE(c);
    EXPECT_FALSE(llvm::verifyModule(*c.mod, nullptr));

    auto* f = c.mod->getFunction("f");
    ASSERT_NE(f, nullptr);

    int revertCalls = 0, unreachables = 0, condBranches = 0;
    for (auto& bb : *f) for (auto& inst : bb) {
        if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
            if (call->getCalledFunction() &&
                call->getCalledFunction()->getName() == "evm.revert") {
                revertCalls++;
            }
        }
        if (llvm::isa<llvm::UnreachableInst>(&inst)) unreachables++;
        if (auto* br = llvm::dyn_cast<llvm::BranchInst>(&inst)) {
            if (br->isConditional()) condBranches++;
        }
    }
    EXPECT_EQ(revertCalls,  1);
    EXPECT_EQ(unreachables, 1);
    EXPECT_GE(condBranches, 1);
}

TEST(CodeGen, MsgSenderAndValueCallIntrinsics) {
    solc::DiagnosticEngine d;
    auto c = compileSrc(
        "contract C {"
        "  function f(address who) public returns (bool) {"
        "    return who == msg.sender;"
        "  }"
        "  function g() public returns (uint256) {"
        "    return msg.value;"
        "  }"
        "}", d);
    ASSERT_TRUE(c);
    EXPECT_FALSE(llvm::verifyModule(*c.mod, nullptr));

    auto countCalls = [&](llvm::Function* f, const char* name) {
        int n = 0;
        if (!f) return n;
        for (auto& bb : *f) for (auto& inst : bb) {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                if (call->getCalledFunction() &&
                    call->getCalledFunction()->getName() == name) n++;
            }
        }
        return n;
    };
    EXPECT_EQ(countCalls(c.mod->getFunction("f"), "evm.msg_sender"), 1);
    EXPECT_EQ(countCalls(c.mod->getFunction("g"), "evm.msg_value"),  1);
}

TEST(CodeGen, CounterFullVerifies) {
    std::ifstream f(std::string(SOLC_TESTS_DIR) + "/fixtures/counter.sol");
    std::stringstream ss; ss << f.rdbuf();
    std::string src = ss.str();
    ASSERT_FALSE(src.empty());

    solc::DiagnosticEngine d;
    auto compiled = compileSrc(src.c_str(), d);
    ASSERT_TRUE((bool)compiled);
    EXPECT_FALSE(d.hasErrors());
    EXPECT_FALSE(llvm::verifyModule(*compiled.mod, nullptr));

    EXPECT_NE(compiled.mod->getFunction("increment"),     nullptr);
    EXPECT_NE(compiled.mod->getFunction("isOwner"),       nullptr);
    EXPECT_NE(compiled.mod->getFunction("evm.revert"),    nullptr);
    EXPECT_NE(compiled.mod->getFunction("evm.msg_sender"),nullptr);
}

TEST(CodeGen, CounterNoRequireVerifies) {
    std::ifstream f(std::string(SOLC_TESTS_DIR) + "/fixtures/counter_no_require.sol");
    std::stringstream ss; ss << f.rdbuf();
    std::string src = ss.str();
    ASSERT_FALSE(src.empty());

    solc::DiagnosticEngine d;
    auto compiled = compileSrc(src.c_str(), d);
    ASSERT_TRUE((bool)compiled);
    EXPECT_FALSE(d.hasErrors());
    EXPECT_FALSE(llvm::verifyModule(*compiled.mod, nullptr));

    EXPECT_NE(compiled.mod->getFunction("increment"), nullptr);
    EXPECT_NE(compiled.mod->getFunction("isPositive"), nullptr);
}

// --- внешний вызов и reentrancy-паттерн (Этап 0) ---

static int countExternalCalls(llvm::Function* f) {
    int n = 0;
    for (auto& bb : *f) for (auto& inst : bb)
        if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst))
            if (call->getCalledFunction() &&
                call->getCalledFunction()->getName() == "scsa.external_call") n++;
    return n;
}

TEST(CodeGen, LowLevelCallEmitsExternalCallWithMetadata) {
    solc::DiagnosticEngine d;
    auto c = compileSrc(
        "contract C {"
        "  function f() public { msg.sender.call{value: 1}(\"\"); }"
        "}", d);
    ASSERT_TRUE(c);
    EXPECT_FALSE(llvm::verifyModule(*c.mod, nullptr));
    auto* f = c.mod->getFunction("f");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(countExternalCalls(f), 1);

    // метки вызова: тип и уровень доверия — вход для reentrancy-детектора
    bool tagged = false;
    for (auto& bb : *f) for (auto& inst : bb)
        if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst))
            if (call->getCalledFunction() &&
                call->getCalledFunction()->getName() == "scsa.external_call")
                tagged = call->getMetadata("scsa.call.kind") &&
                         call->getMetadata("scsa.call.trusted");
    EXPECT_TRUE(tagged);
}

// ядро артефакта Этапа 0: внешний вызов ПРЕДШЕСТВУЕТ storage-записи
// в одном базовом блоке — структурная сигнатура reentrancy
TEST(CodeGen, ReentrancyExternalCallPrecedesStorageStore) {
    std::ifstream f(std::string(SOLC_TESTS_DIR) + "/fixtures/reentrancy.sol");
    std::stringstream ss; ss << f.rdbuf();
    std::string src = ss.str();
    ASSERT_FALSE(src.empty());

    solc::DiagnosticEngine d;
    auto compiled = compileSrc(src.c_str(), d);
    ASSERT_TRUE((bool)compiled);
    EXPECT_FALSE(d.hasErrors());
    EXPECT_FALSE(llvm::verifyModule(*compiled.mod, nullptr));

    auto* wd = compiled.mod->getFunction("withdraw");
    ASSERT_NE(wd, nullptr);
    EXPECT_EQ(countExternalCalls(wd), 1);

    // проходим инструкции по порядку: сначала должен встретиться external_call,
    // затем store в addrspace(1) — иначе это не искомый паттерн
    bool sawExtCall = false, storeAfterCall = false;
    for (auto& bb : *wd) for (auto& inst : bb) {
        if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst))
            if (call->getCalledFunction() &&
                call->getCalledFunction()->getName() == "scsa.external_call")
                sawExtCall = true;
        if (auto* st = llvm::dyn_cast<llvm::StoreInst>(&inst))
            if (st->getPointerAddressSpace() == 1 && sawExtCall)
                storeAfterCall = true;
    }
    EXPECT_TRUE(storeAfterCall);
}

// Этап 4.5: новый синтаксис боевого Solidity компилируется в валидный IR —
// var-локаль, модуло %, тернарник ?:, new C() как инструкция. verifyModule должен
// пройти (все коэрции ширины на месте). Касты Contract(x)/безтеловые прототипы
// покрыты независимым клоном (honeypot/spank), см. benchmarks/RESULTS.md
TEST(CodeGen, WartimeSyntaxCompiles) {
    solc::DiagnosticEngine d;
    auto c = compileSrc(
        "contract W {"
        "  mapping(address => uint256) bal;"
        "  function f(uint256 x) public returns (uint256) {"
        "    var y = bal[msg.sender];"           // var + storage-copy
        "    uint256 z = x % 7;"                  // модуло
        "    uint256 w = x > z ? x : z;"          // тернарник
        "    new Widget();"                       // new → opaque выражение-инструкция
        "    return y + z + w;"
        "  } }",
        d);
    ASSERT_TRUE((bool)c);
    EXPECT_FALSE(llvm::verifyModule(*c.mod, nullptr));
}
