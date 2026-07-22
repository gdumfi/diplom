#include "solc/analyzer/IRTrace.h"

#include <unordered_set>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>

namespace solc::analyzer::trace {
namespace {

using Seen = std::unordered_set<const llvm::Value*>;

bool reachesCallImpl(const llvm::Value* v, llvm::StringRef name, Seen& seen) {
    if (auto* c = llvm::dyn_cast<llvm::CallInst>(v)) {
        auto* fn = c->getCalledFunction();
        return fn && fn->getName() == name;
    }
    const auto* inst = llvm::dyn_cast<llvm::Instruction>(v);
    if (!inst || !seen.insert(v).second) return false;
    if (llvm::isa<llvm::TruncInst>(inst) || llvm::isa<llvm::ZExtInst>(inst))
        return reachesCallImpl(inst->getOperand(0), name, seen);
    return false;
}

bool reachesLoadImpl(const llvm::Value* v, int& slot, Seen& seen, bool includeMapping,
                     bool deep = false) {
    if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(v)) {
        if (ld->getPointerAddressSpace() != 1) return false;
        // mapping-элемент (указатель — @scsa.map_elem): per-user состояние.
        // для owner-трассировки исключаем, для cross-function reentrancy — включаем
        if (!includeMapping)
            if (auto* c = llvm::dyn_cast<llvm::CallInst>(ld->getPointerOperand()))
                if (c->getCalledFunction() && c->getCalledFunction()->getName() == "scsa.map_elem")
                    return false;
        slot = storageSlotOf(ld);
        return true;
    }
    const auto* inst = llvm::dyn_cast<llvm::Instruction>(v);
    if (!inst || !seen.insert(v).second) return false;
    if (llvm::isa<llvm::TruncInst>(inst) || llvm::isa<llvm::ZExtInst>(inst))
        return reachesLoadImpl(inst->getOperand(0), slot, seen, includeMapping, deep);
    // deep: проходим и арифметику — value вызова часто balance*price/1e18, обычная трасса рвётся
    if (deep) {
        unsigned op = inst->getOpcode();
        if (op == llvm::Instruction::Add || op == llvm::Instruction::Sub ||
            op == llvm::Instruction::Mul || op == llvm::Instruction::UDiv ||
            op == llvm::Instruction::SDiv || op == llvm::Instruction::URem ||
            op == llvm::Instruction::SRem)
            for (const auto& o : inst->operands())
                if (reachesLoadImpl(o.get(), slot, seen, includeMapping, deep)) return true;
    }
    return false;
}

bool gatesRevertImpl(const llvm::Value* v, Seen& seen) {
    if (!seen.insert(v).second) return false;
    for (const llvm::User* u : v->users()) {
        if (const auto* br = llvm::dyn_cast<llvm::BranchInst>(u)) {
            if (!br->isConditional() || br->getCondition() != v) continue;
            for (unsigned i = 0; i < br->getNumSuccessors(); ++i)
                if (isRevertBlock(br->getSuccessor(i))) return true;
        } else if (const auto* bin = llvm::dyn_cast<llvm::BinaryOperator>(u)) {
            // && / || компилируются в and/or i1 без short-circuit: следуем за результатом
            if ((bin->getOpcode() == llvm::Instruction::And ||
                 bin->getOpcode() == llvm::Instruction::Or) &&
                gatesRevertImpl(bin, seen))
                return true;
        }
    }
    return false;
}

}  // namespace

int storageSlotOf(const llvm::Instruction* inst) {
    if (auto* md = inst->getMetadata("scsa.storage"))
        if (md->getNumOperands() == 1)
            if (auto* cam = llvm::dyn_cast<llvm::ConstantAsMetadata>(md->getOperand(0)))
                if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(cam->getValue()))
                    return static_cast<int>(ci->getSExtValue());
    return -1;
}

bool reachesCall(const llvm::Value* v, llvm::StringRef calleeName) {
    Seen seen;
    return reachesCallImpl(v, calleeName, seen);
}

bool reachesStorageLoad(const llvm::Value* v, int& slot) {
    Seen seen;
    return reachesLoadImpl(v, slot, seen, /*includeMapping=*/false);
}

bool reachesStorageLoadAny(const llvm::Value* v, int& slot) {
    Seen seen;
    return reachesLoadImpl(v, slot, seen, /*includeMapping=*/true);
}

bool reachesStorageLoadDeep(const llvm::Value* v, int& slot) {
    Seen seen;
    return reachesLoadImpl(v, slot, seen, /*includeMapping=*/true, /*deep=*/true);
}

bool isRevertBlock(const llvm::BasicBlock* bb) {
    for (const auto& inst : *bb) {
        if (auto* c = llvm::dyn_cast<llvm::CallInst>(&inst)) {
            auto* fn = c->getCalledFunction();
            if (fn && fn->getName() == "evm.revert") return true;
        }
        return false;
    }
    return false;
}

bool gatesRevert(const llvm::Value* v) {
    Seen seen;
    return gatesRevertImpl(v, seen);
}

bool hasMsgSenderGate(const llvm::Function& fn) {
    for (const auto& bb : fn)
        for (const auto& inst : bb) {
            const auto* ic = llvm::dyn_cast<llvm::ICmpInst>(&inst);
            if (!ic) continue;
            if (!reachesCall(ic->getOperand(0), "evm.msg_sender") &&
                !reachesCall(ic->getOperand(1), "evm.msg_sender"))
                continue;
            if (gatesRevert(ic)) return true;
        }
    return false;
}

std::vector<std::string> functionParamAbiTypes(const llvm::Function& fn) {
    std::vector<std::string> out;
    if (auto* md = fn.getMetadata("scsa.abi"))
        for (const auto& op : md->operands())
            if (auto* s = llvm::dyn_cast<llvm::MDString>(op.get()))
                out.push_back(s->getString().str());
    return out;
}

std::unordered_set<const llvm::Function*> collectMsgSenderFns(const llvm::Module& m) {
    std::unordered_set<const llvm::Function*> s;
    for (const llvm::Function& fn : m) {
        if (fn.isDeclaration()) continue;
        for (const auto& bb : fn)
            if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(bb.getTerminator()))
                if (ret->getReturnValue() && reachesCall(ret->getReturnValue(), "evm.msg_sender")) {
                    s.insert(&fn); break;
                }
    }
    return s;
}

bool reachesMsgSenderX(const llvm::Value* v,
                       const std::unordered_set<const llvm::Function*>& ms) {
    // прямой msg.sender (через trunc/zext уже умеет reachesCall)
    if (reachesCall(v, "evm.msg_sender")) return true;
    // либо вызов msg.sender-возвращающей функции (в т.ч. под приведением ширины)
    const llvm::Value* cur = v;
    for (int i = 0; i < 8 && cur; ++i) {
        if (auto* c = llvm::dyn_cast<llvm::CallInst>(cur))
            if (auto* f = c->getCalledFunction()) if (ms.count(f)) return true;
        if (auto* inst = llvm::dyn_cast<llvm::Instruction>(cur);
            inst && (llvm::isa<llvm::TruncInst>(inst) || llvm::isa<llvm::ZExtInst>(inst)))
            cur = inst->getOperand(0);
        else break;
    }
    return false;
}

// условие ветвления может быть КОМБИНАЦИЕЙ сравнений: `msg.sender == owner() || msg.sender == keeper`
// (OZ/Beefy `_checkManager`) даёт `or i1 (icmp),(icmp)` — не прямой ICmp. рекурсивно ищем icmp с
// msg.sender сквозь and/or/xor. без этого guard за owner()-функцией + OR не распознавался → FP на
// setKeeper и т.п. (Beefy: 15 ложных access-control)
static bool condHasMsgSenderCmp(const llvm::Value* v,
                                const std::unordered_set<const llvm::Function*>& ms, int depth) {
    if (!v || depth > 6) return false;
    if (const auto* ic = llvm::dyn_cast<llvm::ICmpInst>(v))
        return reachesMsgSenderX(ic->getOperand(0), ms) || reachesMsgSenderX(ic->getOperand(1), ms);
    if (const auto* bo = llvm::dyn_cast<llvm::BinaryOperator>(v))
        return condHasMsgSenderCmp(bo->getOperand(0), ms, depth + 1) ||
               condHasMsgSenderCmp(bo->getOperand(1), ms, depth + 1);
    return false;
}

bool isAuthGuardFn(const llvm::Function& fn,
                   const std::unordered_set<const llvm::Function*>& ms) {
    for (const auto& bb : fn) {
        const auto* br = llvm::dyn_cast<llvm::BranchInst>(bb.getTerminator());
        if (!br || !br->isConditional()) continue;
        bool toRevert = false;
        for (unsigned i = 0; i < br->getNumSuccessors(); ++i)
            if (isRevertBlock(br->getSuccessor(i))) toRevert = true;
        if (!toRevert) continue;
        if (condHasMsgSenderCmp(br->getCondition(), ms, 0))   // прямой icmp ИЛИ and/or-комбинация
            return true;
    }
    return false;
}

}  // namespace solc::analyzer::trace
