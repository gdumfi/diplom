#include <gtest/gtest.h>
#include <memory>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>

#include "solc/codegen/CodeGen.h"
#include "solc/klee/KleeLowering.h"
#include "solc/lexer/Lexer.h"
#include "solc/parser/Parser.h"
#include "solc/sema/Sema.h"

// компилируем исходник в размеченный IR (как codegen), затем понижаем в KLEE-модуль
struct Lowered {
    std::unique_ptr<solc::codegen::CodeGen> cg;
    std::unique_ptr<llvm::Module>           mod;   // KLEE-модуль
    operator bool() const { return mod != nullptr; }
};

static Lowered lowerKlee(const char* src, const char* entry) {
    solc::DiagnosticEngine d;
    solc::Lexer lex(src, d);
    solc::Parser p(lex, d);
    auto c = p.parseContract();
    if (!c) return {};
    solc::sema::SemanticAnalyzer sa(d);
    if (!sa.analyze(*c)) return {};
    auto cg = std::make_unique<solc::codegen::CodeGen>(d);
    auto mod = cg->generate(*c);
    if (!mod) return {};
    solc::klee::KleeLowering low;
    auto km = low.lower(*mod, entry);
    return {std::move(cg), std::move(km)};
}

static int countCalls(const llvm::Module& m, llvm::StringRef name) {
    int n = 0;
    for (auto& f : m)
        for (auto& bb : f)
            for (auto& inst : bb)
                if (auto* c = llvm::dyn_cast<llvm::CallInst>(&inst))
                    if (c->getCalledFunction() && c->getCalledFunction()->getName() == name)
                        n++;
    return n;
}

static const char* kVault =
    "contract Vault {"
    "  uint256 balance;"
    "  function deposit() public { balance = balance + msg.value; }"
    "  function withdraw() public {"
    "    require(balance > 0);"
    "    msg.sender.call{value: balance}(\"\");"
    "    balance = 0;"
    "  } }";

// понижение даёт валидный модуль с харнессом main
TEST(Klee, LowersToValidModuleWithMain) {
    auto l = lowerKlee(kVault, "withdraw");
    ASSERT_TRUE((bool)l);
    EXPECT_FALSE(llvm::verifyModule(*l.mod, nullptr));
    EXPECT_NE(l.mod->getFunction("main"), nullptr);
}

// модуль самодостаточен: не осталось внешних @evm.*/@scsa.* символов
TEST(Klee, NoLeftoverEvmScsaSymbols) {
    auto l = lowerKlee(kVault, "withdraw");
    ASSERT_TRUE((bool)l);
    for (auto& f : *l.mod) {
        EXPECT_FALSE(f.getName().starts_with("evm."))  << f.getName().str();
        EXPECT_FALSE(f.getName().starts_with("scsa.")) << f.getName().str();
    }
}

// контекст символизируется, revert -> klee_silent_exit
TEST(Klee, SymbolizesContextAndLowersRevert) {
    auto l = lowerKlee(kVault, "withdraw");
    ASSERT_TRUE((bool)l);
    // klee_make_symbolic: msg_sender/msg_value/tx_origin/block_timestamp (>=4)
    EXPECT_GE(countCalls(*l.mod, "klee_make_symbolic"), 4);
    // require(balance>0) провалившись -> silent_exit
    EXPECT_GE(countCalls(*l.mod, "klee_silent_exit"), 1);
}

// storage-доступ переведён в реальную память (addrspace(1) global), а не inttoptr
TEST(Klee, StorageBackedByGlobal) {
    auto l = lowerKlee(kVault, "withdraw");
    ASSERT_TRUE((bool)l);
    EXPECT_NE(l.mod->getNamedGlobal("scsa.storage"), nullptr);
    // ни одной оставшейся external_call-заглушки
    EXPECT_EQ(countCalls(*l.mod, "scsa.external_call"), 0);
    // storage-доступы не должны висеть на null (inttoptr(0) свёрнут в null) —
    // все load/store storage обязаны идти в глобаль scsa.storage
    for (auto& f : *l.mod)
        for (auto& bb : f)
            for (auto& inst : bb) {
                const llvm::Value* p = nullptr;
                if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(&inst)) p = ld->getPointerOperand();
                if (auto* st = llvm::dyn_cast<llvm::StoreInst>(&inst)) p = st->getPointerOperand();
                if (p) EXPECT_FALSE(llvm::isa<llvm::ConstantPointerNull>(p));
            }
}

// K1.1: fork реентрантного callback — target вызывает сам себя в extcall.reenter,
// глубину ограничивает счётчик scsa.reentry_depth (§6.2, ядро метода)
TEST(Klee, ReentrancyCallbackFork) {
    auto l = lowerKlee(kVault, "withdraw");
    ASSERT_TRUE((bool)l);
    EXPECT_FALSE(llvm::verifyModule(*l.mod, nullptr));
    // счётчик глубины
    EXPECT_NE(l.mod->getNamedGlobal("scsa.reentry_depth"), nullptr);
    // блок реентрантного callback
    auto* wd = l.mod->getFunction("withdraw");
    ASSERT_NE(wd, nullptr);
    bool hasReenterBB = false;
    int selfCalls = 0;
    for (auto& bb : *wd) {
        if (bb.getName().starts_with("extcall.reenter")) hasReenterBB = true;
        for (auto& inst : bb)
            if (auto* c = llvm::dyn_cast<llvm::CallInst>(&inst))
                if (c->getCalledFunction() == wd) selfCalls++;   // withdraw зовёт withdraw
    }
    EXPECT_TRUE(hasReenterBB);
    EXPECT_GE(selfCalls, 1);
}

// K1.2: оракул кражи — учёт отправленного (total_sent) + klee_report_error на пути,
// где наружу ушло больше исходного баланса. KLEE, найдя такой путь, сообщит о баге
TEST(Klee, TheftOracle) {
    auto l = lowerKlee(kVault, "withdraw");
    ASSERT_TRUE((bool)l);
    EXPECT_FALSE(llvm::verifyModule(*l.mod, nullptr));
    EXPECT_NE(l.mod->getNamedGlobal("scsa.total_sent"), nullptr);
    // отчёт об ошибке присутствует и вызывается в харнессе
    auto* re = l.mod->getFunction("klee_report_error");
    ASSERT_NE(re, nullptr);
    EXPECT_GE(countCalls(*l.mod, "klee_report_error"), 1);
    // блок violation (нарушение оракула) в main
    auto* mainFn = l.mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    bool hasViolation = false;
    for (auto& bb : *mainFn) if (bb.getName().starts_with("violation")) hasViolation = true;
    EXPECT_TRUE(hasViolation);
}

// access-control: понижение в режиме Mode::AccessControl даёт валидный модуль с флагом
// priv_written и оракулом non-owner (символьное подтверждение денежной ставки #1, $953M)
TEST(Klee, AccessControlLowers) {
    solc::DiagnosticEngine d;
    solc::Lexer lex(
        "contract B {"
        "  address owner;"
        "  uint256 c;"
        "  function p(uint256 x) public { require(msg.sender == owner); c = x; }"
        "  function setOwner(address o) public { owner = o; } }", d);
    solc::Parser p(lex, d);
    auto contract = p.parseContract();
    ASSERT_TRUE((bool)contract);
    solc::sema::SemanticAnalyzer sa(d);
    ASSERT_TRUE(sa.analyze(*contract));
    auto cg = std::make_unique<solc::codegen::CodeGen>(d);
    auto mod = cg->generate(*contract);
    ASSERT_NE(mod, nullptr);

    solc::klee::KleeLowering low;
    // owner — слот 0 (первая state var), setOwner пишет его без проверки → privSlot=ownerSlot=0
    auto km = low.lower(*mod, "setOwner", /*privSlot=*/0,
                        solc::klee::Mode::AccessControl, /*ownerSlot=*/0);
    ASSERT_NE(km, nullptr);
    EXPECT_FALSE(llvm::verifyModule(*km, nullptr));
    EXPECT_NE(km->getNamedGlobal("scsa.priv_written"), nullptr);
    EXPECT_NE(km->getFunction("main"), nullptr);
    for (auto& f : *km) EXPECT_FALSE(f.getName().starts_with("evm.")) << f.getName().str();
}

// block.number символизируется (§4.3), а не opaque 0 — иначе гарды с block.number
// схлопываются в ложь и путь reentrancy становится ложно-недостижимым
TEST(Klee, SymbolizesBlockNumber) {
    auto l = lowerKlee(
        "contract B {"
        "  mapping(address => uint256) bal;"
        "  uint256 lastBlock;"
        "  function cashOut(uint256 a) public {"
        "    if (a <= bal[msg.sender] && block.number > lastBlock) {"
        "      msg.sender.call{value: a}(\"\");"
        "      bal[msg.sender] = bal[msg.sender] - a;"
        "    }"
        "  } }",
        "cashOut");
    ASSERT_TRUE((bool)l);
    EXPECT_FALSE(llvm::verifyModule(*l.mod, nullptr));
    EXPECT_NE(l.mod->getNamedGlobal("sym.block_number"), nullptr);
    // ни одного остаточного @evm.block_number
    EXPECT_EQ(l.mod->getFunction("evm.block_number"), nullptr);
}

// понижение работает и для mapping-версии (map_elem -> storage global)
TEST(Klee, LowersMappingReentrancy) {
    auto l = lowerKlee(
        "contract Bank {"
        "  mapping(address => uint256) bal;"
        "  function withdraw() public {"
        "    require(bal[msg.sender] > 0);"
        "    msg.sender.call{value: bal[msg.sender]}(\"\");"
        "    bal[msg.sender] = 0;"
        "  } }",
        "withdraw");
    ASSERT_TRUE((bool)l);
    EXPECT_FALSE(llvm::verifyModule(*l.mod, nullptr));
    EXPECT_EQ(countCalls(*l.mod, "scsa.map_elem"), 0);
    EXPECT_NE(l.mod->getNamedGlobal("scsa.storage"), nullptr);
}
