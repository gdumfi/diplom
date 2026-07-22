#include "solc/analyzer/ReentrancyDetector.h"

#include <optional>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "solc/analyzer/IRTrace.h"

#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>

namespace solc::analyzer {
namespace {

// вызов @scsa.external_call или nullptr
const llvm::CallInst* asExternalCall(const llvm::Instruction& inst) {
    if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
        auto* fn = call->getCalledFunction();
        if (fn && fn->getName() == "scsa.external_call") return call;
    }
    return nullptr;
}

// вызов может передать управление недоверенному коду (возможен реентрантный callback):
// ETH-value>0 (классика) ИЛИ недоверенный не-static вызов — .call(data)/delegatecall,
// т.е. токен-хук (ERC777/721) или произвольный callback. staticcall и доверенный target
// исключены (сохранение precision). расширяет read-only reentrancy на call-триггеры.
bool canReenter(const llvm::CallInst* c) {
    if (c->arg_size() >= 2)
        if (auto* cv = llvm::dyn_cast<llvm::ConstantInt>(c->getArgOperand(1)))
            if (!cv->isZero()) return true;               // ETH-value — всегда триггер
    if (auto* md = c->getMetadata("scsa.call.kind"))
        if (md->getNumOperands() > 0)
            if (auto* s = llvm::dyn_cast<llvm::MDString>(md->getOperand(0)))
                if (s->getString() == "staticcall") return false;  // read-only — не триггер
    if (auto* mt = c->getMetadata("scsa.call.trusted"))
        if (mt->getNumOperands() > 0)
            if (auto* ci = llvm::mdconst::dyn_extract<llvm::ConstantInt>(mt->getOperand(0)))
                if (!ci->isZero()) return false;          // доверенный target
    return true;                                          // недоверенный не-static вызов
}

// запись в storage (addrspace 1) или nullptr
const llvm::StoreInst* asStorageStore(const llvm::Instruction& inst) {
    if (auto* st = llvm::dyn_cast<llvm::StoreInst>(&inst))
        if (st->getPointerAddressSpace() == 1) return st;
    return nullptr;
}

// чтение из storage (addrspace 1) или nullptr
const llvm::LoadInst* asStorageLoad(const llvm::Instruction& inst) {
    if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(&inst))
        if (ld->getPointerAddressSpace() == 1) return ld;
    return nullptr;
}

// слот из метаданного !scsa.storage !{i64 slot}, либо -1.
// работает и для store, и для load — метка одинаковая
int storageSlotOf(const llvm::Instruction* inst) {
    if (auto* md = inst->getMetadata("scsa.storage"))
        if (md->getNumOperands() == 1)
            if (auto* cam = llvm::dyn_cast<llvm::ConstantAsMetadata>(md->getOperand(0)))
                if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(cam->getValue()))
                    return static_cast<int>(ci->getSExtValue());
    return -1;
}

// существует ли путь в CFG, на котором store исполняется ПОСЛЕ call.
// это reachability (exists-a-path), а не доминирование — именно она нужна для
// reentrancy: достаточно ОДНОГО пути, где storage пишется после внешнего вызова
bool instrReachableAfterCall(const llvm::CallInst* call, const llvm::Instruction* target) {
    const llvm::BasicBlock* cb = call->getParent();
    const llvm::BasicBlock* tb = target->getParent();

    if (cb == tb) {
        // один блок: target после call по порядку инструкций
        for (auto it = call->getIterator(), end = cb->end(); it != end; ++it)
            if (&*it == target) return true;
        // target раньше call: «после» он оказался бы только через цикл,
        // а в подмножестве MVP циклов нет — значит не после
        return false;
    }
    // разные блоки: достижим ли tb из cb по рёбрам-успешникам
    std::unordered_set<const llvm::BasicBlock*> seen;
    std::queue<const llvm::BasicBlock*> q;
    for (const auto* s : llvm::successors(cb)) q.push(s);
    while (!q.empty()) {
        const llvm::BasicBlock* bb = q.front();
        q.pop();
        if (bb == tb) return true;
        if (!seen.insert(bb).second) continue;
        for (const auto* s : llvm::successors(bb)) q.push(s);
    }
    return false;
}
bool storeReachableAfterCall(const llvm::CallInst* call, const llvm::StoreInst* store) {
    return instrReachableAfterCall(call, store);
}

// строка эффекта: приоритет у storage-записи (места, где реально нарушается
// checks-effects-interactions), при отсутствии !dbg берём строку внешнего
// вызова, иначе -1. так номер указывает на строку, которую правит разработчик
int findingLine(const llvm::CallInst* call, const llvm::StoreInst* store) {
    if (store)
        if (const llvm::DebugLoc& dl = store->getDebugLoc()) return dl.getLine();
    if (call)
        if (const llvm::DebugLoc& dl = call->getDebugLoc()) return dl.getLine();
    return -1;
}

// исполняется ли инструкция a строго ДО инструкции b хотя бы на одном пути CFG.
// нужно, чтобы отличить guard (до вызова) от освобождения флага (после вызова)
bool executesBefore(const llvm::Instruction* a, const llvm::Instruction* b) {
    const llvm::BasicBlock* ab = a->getParent();
    const llvm::BasicBlock* bb = b->getParent();
    if (ab == bb) {
        for (auto it = a->getIterator(), end = ab->end(); it != end; ++it)
            if (&*it == b) return true;
        return false;
    }
    // b в другом блоке: достижим ли блок b из блока a по успешникам
    std::unordered_set<const llvm::BasicBlock*> seen;
    std::queue<const llvm::BasicBlock*> q;
    for (const auto* s : llvm::successors(ab)) q.push(s);
    while (!q.empty()) {
        const llvm::BasicBlock* cur = q.front();
        q.pop();
        if (cur == bb) return true;
        if (!seen.insert(cur).second) continue;
        for (const auto* s : llvm::successors(cur)) q.push(s);
    }
    return false;
}

// блок, который сразу же реверзит: первая же инструкция — call @evm.revert.
// codegen так генерирует ветку require.fail (см. CodeGen::emitStmt Require)
bool isRevertBlock(const llvm::BasicBlock* bb) {
    for (const auto& inst : *bb) {
        if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
            auto* fn = call->getCalledFunction();
            if (fn && fn->getName() == "evm.revert") return true;
        }
        // до revert допускаем только служебное; иное — это уже не «чистый» fail-блок
        return false;
    }
    return false;
}

// чётность логических отрицаний (`!` = xor i1 с 1) на пути от условия v к
// загрузке target. возвращает: 0 — условие сводится к самому флагу (require(flag)),
// 1 — к отрицанию флага (require(!flag)); nullopt — условие НЕ является простой
// цепочкой trunc/zext/`!` от target (тогда это не канонический guard).
//
// зачем полярность: require(!locked) — настоящий lock (проходим, только если НЕ
// заблокировано), а require(locked) — обычный бизнес-флаг, НЕ защита. без этого
// различия детектор молча понижал бы реальную уязвимость (см. контрпример Bank).
std::optional<int> negationParity(const llvm::Value* v, const llvm::LoadInst* target) {
    if (v == target) return 0;
    const auto* inst = llvm::dyn_cast<llvm::Instruction>(v);
    if (!inst) return std::nullopt;
    // xor(x, 1) — логическое «не» для bool: инвертируем чётность
    if (inst->getOpcode() == llvm::Instruction::Xor) {
        const auto* a = inst->getOperand(0);
        const auto* b = inst->getOperand(1);
        const auto* ca = llvm::dyn_cast<llvm::ConstantInt>(a);
        const auto* cb = llvm::dyn_cast<llvm::ConstantInt>(b);
        if (cb && cb->isOne()) { auto p = negationParity(a, target); if (p) return (*p + 1) & 1; }
        else if (ca && ca->isOne()) { auto p = negationParity(b, target); if (p) return (*p + 1) & 1; }
        return std::nullopt;
    }
    // приведения ширины прозрачны для полярности (bool <-> i256)
    if (llvm::isa<llvm::TruncInst>(inst) || llvm::isa<llvm::ZExtInst>(inst))
        return negationParity(inst->getOperand(0), target);
    // && / || (and/or i1 без short-circuit): флаг может быть конъюнктом условия
    // (require(!locked && cond)). ищем полярность в любом из операндов
    if (inst->getOpcode() == llvm::Instruction::And ||
        inst->getOpcode() == llvm::Instruction::Or) {
        if (auto p = negationParity(inst->getOperand(0), target)) return p;
        return negationParity(inst->getOperand(1), target);
    }
    return std::nullopt;
}

// сравнивается ли load target с КОНСТАНТОЙ в условии (icmp eq/ne через trunc/zext).
// это equality-lock: require(locked==1) / require(_status!=_ENTERED) (Solmate/OZ
// ReentrancyGuard). бизнес-флаг require(active) сюда НЕ попадает (нет сравнения с константой)
bool comparesLoadToConst(const llvm::Value* cond, const llvm::LoadInst* target) {
    const auto* ic = llvm::dyn_cast<llvm::ICmpInst>(cond);
    if (!ic) return false;
    auto isTarget = [&](const llvm::Value* v) {
        while (const auto* i = llvm::dyn_cast<llvm::Instruction>(v)) {
            if (v == target) return true;
            if (llvm::isa<llvm::TruncInst>(i) || llvm::isa<llvm::ZExtInst>(i)) v = i->getOperand(0);
            else break;
        }
        return v == target;
    };
    const auto* a = ic->getOperand(0);
    const auto* b = ic->getOperand(1);
    return (isTarget(a) && llvm::isa<llvm::ConstantInt>(b)) ||
           (isTarget(b) && llvm::isa<llvm::ConstantInt>(a));
}

// есть ли в функции настоящий nonReentrant-guard для внешнего вызова call.
//
// распознаём канонический lock: require(!flag) (негация) ИЛИ equality-lock
//   require(locked==1) / require(_status!=_ENTERED) (Solmate/OZ ReentrancyGuard):
//   flag = true;      // 2. set-store того же слота g ПОСЛЕ check-load и ДО вызова
//   ... call ...
// требуется всё сразу для одного слота g:
//   (1) условная ветка require ведёт в revert-блок;
//   (2) её условие сводится к ОТРИЦАНИЮ load-а слота g (negationParity == 1) —
//       именно require(!flag), а не require(flag);
//   (3) есть store в слот g, исполняемый ПОСЛЕ этого check-load и ДО вызова.
//
// почему так строго: раньше проверялось лишь «слот записан и как-то участвует в
// require» — под это подходил обычный бизнес-флаг (require(active)), и реальная
// уязвимость на ДРУГОМ слоте молча понижалась до LOW (ложный негатив, худшее для
// баунти). полярность + порядок отсекают такие подделки — они остаются HIGH.
//
// границы (честно): ловим только линейный паттерн из codegen текущего подмножества.
// НЕ распознаём guard в модификаторе/другой функции, флаги в маппингах, нетривиальные
// условия. если паттерн не опознан — guard считается ОТСУТСТВУЮЩИМ, находка HIGH.
//
// ВАЖНО: check-load и set-store должны ДОМИНИРОВАТЬ вызов (исполняться на ВСЕХ путях
// к нему), а не просто на каком-то пути. иначе guard, взводимый в ветке if, ложно
// подавил бы реальный баг (условно взведённый флаг сам по себе некорректен).
bool isGuardedCall(const llvm::Function& fn, const llvm::CallInst* call, int& guardSlot,
                   const llvm::DominatorTree& DT) {
    for (const auto& bb : fn) {
        const auto* rawTerm = bb.getTerminator();
        if (!rawTerm) continue;
        const auto* term = llvm::dyn_cast<llvm::BranchInst>(rawTerm);
        if (!term || !term->isConditional()) continue;
        // одна из ветвей ведёт в revert-блок — это require
        bool guardsRevert = false;
        for (unsigned i = 0; i < term->getNumSuccessors(); ++i)
            if (isRevertBlock(term->getSuccessor(i))) { guardsRevert = true; break; }
        if (!guardsRevert) continue;

        const llvm::Value* cond = term->getCondition();
        // ищем check-load слота g, к ОТРИЦАНИЮ которого сводится условие;
        // check-load обязан доминировать вызов (require реально на всех путях)
        for (const auto& b2 : fn)
            for (const auto& i2 : b2) {
                const auto* ld = asStorageLoad(i2);
                if (!ld || !DT.dominates(ld, call)) continue;
                int slot = storageSlotOf(ld);
                if (slot < 0) continue;
                // guard-check: либо require(!flag) (негация), либо equality-lock
                // require(locked==1)/require(_status!=_ENTERED) (Solmate/OZ ReentrancyGuard) —
                // сравнение флага с КОНСТАНТОЙ. бизнес-флаг require(active) (без сравнения с
                // константой) НЕ распознаём → он остаётся HIGH
                auto parity = negationParity(cond, ld);
                bool negForm = parity && *parity == 1;
                if (!negForm && !comparesLoadToConst(cond, ld)) continue;

                // set-store того же слота ПОСЛЕ check-load и ДОМИНИРУЮЩИЙ вызов
                for (const auto& b3 : fn)
                    for (const auto& i3 : b3) {
                        const auto* st = asStorageStore(i3);
                        if (!st || storageSlotOf(st) != slot) continue;
                        if (!executesBefore(ld, st) || !DT.dominates(st, call)) continue;

                        // set-store должен ВЗВОДИТЬ флаг — писать истину (ненулевую
                        // константу). запись false (locked=false, значение 0) или
                        // непостоянного значения флаг не взводит и guard-ом не является
                        const auto* sv = llvm::dyn_cast<llvm::ConstantInt>(st->getValueOperand());
                        if (!sv || sv->isZero()) continue;

                        // set-store должен быть ПОСЛЕДНЕЙ записью флага до вызова:
                        // если между ним и вызовом флаг ещё раз пишется (напр.
                        // locked = false — снятиеguard до call), защита не действует
                        // на момент вызова → это НЕ guard, оставляем находку HIGH
                        bool overridden = false;
                        for (const auto& b4 : fn)
                            for (const auto& i4 : b4) {
                                const auto* st2 = asStorageStore(i4);
                                if (!st2 || st2 == st || storageSlotOf(st2) != slot) continue;
                                if (executesBefore(st, st2) && executesBefore(st2, call))
                                    overridden = true;
                            }
                        if (overridden) continue;

                        guardSlot = slot;
                        return true;
                    }
            }
    }
    return false;
}

// вызов @evm.msg_value внутри функции — признак приёма средств (payable-депозит)
bool usesMsgValue(const llvm::Function& fn) {
    for (const auto& bb : fn)
        for (const auto& inst : bb)
            if (auto* c = llvm::dyn_cast<llvm::CallInst>(&inst))
                if (c->getCalledFunction() &&
                    c->getCalledFunction()->getName() == "evm.msg_value")
                    return true;
    return false;
}

// имя Solidity-типа по llvm-типу параметра — для типизированных заглушек в PoC
std::string solTypeName(const llvm::Type* t) {
    if (t->isIntegerTy(160)) return "address";
    if (t->isIntegerTy(1)) return "bool";
    return "uint256";
}

// функция-источник средств для PoC: пополняет ТОТ ЖЕ storage-слот через msg.value
// (кандидат на депозит перед атакой). возвращает имя или "", число и типы параметров
std::string findFundingFunction(const llvm::Module& m, const llvm::Function& vuln,
                                int slot, int& paramCount, std::vector<std::string>& paramTypes) {
    for (const auto& fn : m) {
        if (fn.isDeclaration() || &fn == &vuln) continue;
        if (!usesMsgValue(fn)) continue;
        bool writesSlot = false;
        for (const auto& bb : fn)
            for (const auto& inst : bb)
                if (auto* st = asStorageStore(inst))
                    if (storageSlotOf(st) == slot) writesSlot = true;
        if (writesSlot) {
            paramCount = static_cast<int>(fn.arg_size());
            auto abi = trace::functionParamAbiTypes(fn);   // реальные ABI-типы из solc-AST
            if (!abi.empty()) paramTypes = abi;
            else for (const auto& arg : fn.args()) paramTypes.push_back(solTypeName(arg.getType()));
            return fn.getName().str();
        }
    }
    paramCount = 0;
    return "";
}

// собрать storage-слоты из обратного среза значения v (через load addrspace(1) —
// слот берём из !scsa.storage). нужно, чтобы понять, из каких слотов вычислена
// сумма внешнего вызова
void collectSlotsBackward(const llvm::Value* v, std::unordered_set<int>& slots,
                          std::unordered_set<const llvm::Value*>& seen, int depth = 0) {
    if (!v || depth > 64 || !seen.insert(v).second) return;
    if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(v)) {
        if (ld->getPointerAddressSpace() == 1) {
            int s = storageSlotOf(ld);
            if (s >= 0) slots.insert(s);
        }
        return;   // слот пойман по метке, дальше указателя не идём
    }
    if (auto* inst = llvm::dyn_cast<llvm::Instruction>(v))
        for (const auto& op : inst->operands())
            collectSlotsBackward(op.get(), slots, seen, depth + 1);
}

// «value-relevant» слоты вызова: те, что ОПРЕДЕЛЯЮТ/ГЕЙТЯТ отправляемую сумму —
// обратный срез value-операнда ∪ все storage-слоты, ЧИТАЕМЫЕ до вызова (баланс в
// require/для суммы). запись ПОСЛЕ вызова в слот НЕ отсюда (напр. счётчик) — не
// эффект reentrancy, а безобидная бухгалтерия. если множество пусто (не смогли
// определить) — консервативно считаем все слоты релевантными (без ложных негативов)
std::unordered_set<int> valueRelevantSlots(const llvm::CallInst* call) {
    std::unordered_set<int> slots;
    std::unordered_set<const llvm::Value*> seen;
    if (call->getCalledFunction() &&
        call->getCalledFunction()->getName() == "scsa.external_call" &&
        call->arg_size() >= 2)
        collectSlotsBackward(call->getArgOperand(1), slots, seen);
    for (const auto& bb : *call->getFunction())
        for (const auto& inst : bb)
            if (asStorageLoad(inst) && executesBefore(&inst, call)) {
                int s = storageSlotOf(&inst);
                if (s >= 0) slots.insert(s);
            }
    return slots;
}

}  // namespace

report::Findings ReentrancyDetector::analyze(const llvm::Module& m) {
    report::Findings out;

    // какие storage-слоты пишет какая функция — для cross-function reentrancy;
    // и какие функции содержат внешний вызов — для reentrancy через внутренние вызовы
    std::unordered_map<int, std::unordered_set<const llvm::Function*>> writers;
    std::unordered_set<const llvm::Function*> hasExtCall;
    for (const llvm::Function& fn : m)
        for (const auto& bb : fn)
            for (const auto& inst : bb) {
                if (auto* st = asStorageStore(inst)) {
                    int s = storageSlotOf(st);
                    if (s >= 0) writers[s].insert(&fn);
                }
                if (asExternalCall(inst)) hasExtCall.insert(&fn);
            }

    // «highCapable» функции: делает ли она (транзитивно, через внутренние вызовы) внешний
    // вызов, ОТПРАВЛЯЮЩИЙ ETH-value. нужно, чтобы внутренний вызов-триггер (F зовёт H, а H
    // делает внешний вызов) считался HIGH-триггером reentrancy ТОЛЬКО когда вложенный вызов
    // реально шлёт ETH. вызов доверенного value=0 контракта (IRM/oracle borrowRate, ERC20
    // transferFrom) — не ETH-drain-триггер: иначе КАЖДАЯ функция, зовущая _accrueInterest
    // перед записью учёта, ложно HIGH (Morpho supply/withdraw/borrow/repay/setFee — mainnet)
    std::unordered_map<const llvm::Function*, bool> highCap;
    for (const llvm::Function& fn : m) {
        bool h = false;
        for (const auto& bb : fn)
            for (const auto& inst : bb) {
                const auto* c = asExternalCall(inst);
                if (!c) continue;
                bool zero = false;   // value ДОКАЗУЕМО ноль?
                if (c->arg_size() >= 2)
                    if (auto* cv = llvm::dyn_cast<llvm::ConstantInt>(c->getArgOperand(1)))
                        zero = cv->isZero();
                if (!zero) h = true;   // неконстантный или ненулевой value → возможен ETH-перевод
            }
        highCap[&fn] = h;
    }
    // транзитивное замыкание по внутренним вызовам (функций немного — простой fixpoint)
    for (bool changed = true; changed;) {
        changed = false;
        for (const llvm::Function& fn : m) {
            if (highCap[&fn]) continue;
            for (const auto& bb : fn)
                for (const auto& inst : bb)
                    if (auto* c = llvm::dyn_cast<llvm::CallInst>(&inst))
                        if (auto* callee = c->getCalledFunction())
                            if (callee != &fn && highCap.count(callee) && highCap[callee]) {
                                highCap[&fn] = true;
                                changed = true;
                            }
        }
    }

    for (const llvm::Function& fn : m) {
        if (fn.isDeclaration()) continue;
        // internal/private-функции — не точки входа атаки: reentrancy эксплуатируется через
        // public/external вход (где стоит guard/lock). прямое флагание internal-хелперов даёт
        // FP на роутерах (UniversalRouter: dispatch/v3SwapExactOutput под locked-родителем)
        if (fn.getMetadata("scsa.internal")) continue;

        std::vector<const llvm::CallInst*>  calls;
        std::vector<const llvm::StoreInst*> stores;
        std::vector<const llvm::CallInst*>  opaqueStores;   // @scsa.opaque_store маркеры
        for (const auto& bb : fn)
            for (const auto& inst : bb) {
                if (auto* c = asExternalCall(inst)) {
                    calls.push_back(c);
                } else if (auto* c = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                    auto* callee = c->getCalledFunction();
                    if (callee && callee->getName() == "scsa.opaque_store") opaqueStores.push_back(c);
                    // внутренний вызов функции, которая сама делает внешний вызов —
                    // тоже точка реентрантности (attacker войдёт через callback внутри неё)
                    else if (callee && callee != &fn && hasExtCall.count(callee)) calls.push_back(c);
                }
                if (auto* s = asStorageStore(inst)) stores.push_back(s);
            }
        if (calls.empty()) continue;   // без (внешнего/внутреннего) вызова reentrancy невозможна

        // дерево доминаторов функции — для проверки, что guard на всех путях к вызову
        llvm::DominatorTree DT(const_cast<llvm::Function&>(fn));

        bool fnFlagged = false;
        for (const auto* call : calls) {
            // HIGH классическая reentrancy — только для ETH-value-ОТПРАВЛЯЮЩИХ внешних вызовов
            // (drain: attacker получает ETH и реентрантно выводит снова) ИЛИ внутренних вызовов
            // (inter-procedural). value=0 внешние вызовы (transferFrom pull-in, протокол-вызовы
            // к доверенным контрактам dai/join/pot) НЕ дают классической reentrancy — иначе FP
            // на КАЖДОМ депозите реального DeFi (sDAI/ERC4626 и т.п., проверено на mainnet)
            bool highCapable = true;
            if (call->getCalledFunction() &&
                call->getCalledFunction()->getName() == "scsa.external_call" &&
                call->arg_size() >= 2) {
                if (auto* cv = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(1)))
                    highCapable = !cv->isZero();
            } else if (auto* callee = call->getCalledFunction()) {
                // внутренний вызов-триггер: HIGH только если callee (транзитивно) шлёт ETH-value.
                // value=0 вложенные вызовы (доверенный IRM/oracle/ERC20) не дают ETH-drain (Fix6)
                if (callee->getName() != "scsa.external_call" && highCap.count(callee))
                    highCapable = highCap[callee];
            }
            if (!highCapable) continue;

            // распознаём nonReentrant-guard для этого вызова (см. isGuardedCall)
            int guardSlot = -1;
            bool guarded = isGuardedCall(fn, call, guardSlot, DT);

            // слоты, определяющие/гейтящие сумму вызова. пустое множество =
            // не смогли определить → фильтр не применяем (консервативно)
            std::unordered_set<int> relevant = valueRelevantSlots(call);
            // фильтр применяем ТОЛЬКО к вызовам, отправляющим value наружу (drain-паттерн):
            // тогда «слот, читаемый до вызова» = баланс, а запись в др. слот после — счётчик.
            // для value=0 вызовов (transferFrom pull-in) фильтр НЕ применяем — там запись
            // после (shares += amt) может быть double-credit reentrancy (ERC777), не счётчик
            bool sendsValue = true;
            if (call->getCalledFunction() &&
                call->getCalledFunction()->getName() == "scsa.external_call" &&
                call->arg_size() >= 2) {
                if (auto* cv = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(1)))
                    sendsValue = !cv->isZero();
            } else {
                sendsValue = false;   // внутренний вызов — value неизвестно, не фильтруем
            }

            for (const auto* store : stores) {
                if (!storeReachableAfterCall(call, store)) continue;

                // если функция под guard-ом, запись в слот самого флага после
                // вызова (locked = false) — это штатное освобождение, не эффект
                // нарушения. пропускаем её, чтобы не порождать находку на release
                int slot = storageSlotOf(store);
                if (guarded && slot == guardSlot) continue;

                // precision: для value-отправляющего вызова запись после в слот, НЕ
                // определяющий сумму (напр. счётчик withdrawCount при обнулённом ДО
                // вызова балансе) — не эксплуатируемый эффект. пропускаем, только когда
                // relevant непусто (иначе не уверены → флагаем)
                if (sendsValue && !relevant.empty() && slot >= 0 && !relevant.count(slot)) continue;

                report::Finding f;
                f.rule        = "reentrancy";
                f.function    = fn.getName().str();
                f.storageSlot = slot;
                f.line        = findingLine(call, store);
                // контекст для PoC: параметры уязвимой функции и функция-депозит.
                // реальные ABI-типы из !scsa.abi (uint128/bytes32/…) — верный селектор;
                // fallback на вывод из llvm-типа для legacy-фронтенда
                f.vulnParamCount = static_cast<int>(fn.arg_size());
                if (auto abi = trace::functionParamAbiTypes(fn); !abi.empty())
                    f.vulnParamTypes = abi;
                else
                    for (const auto& arg : fn.args())
                        f.vulnParamTypes.push_back(solTypeName(arg.getType()));
                f.fundingFunction = findFundingFunction(m, fn, slot, f.fundingParamCount,
                                                        f.fundingParamTypes);

                if (guarded) {
                    // guard распознан: реальная запись эффекта под защитой флага.
                    // не подавляем молча — понижаем до LOW с пометкой guarded,
                    // чтобы находка осталась видимой, но не пугала как критичная
                    f.severity = "LOW";
                    f.message =
                        "внешний вызов до записи в storage (слот "
                        + std::to_string(slot)
                        + "), но функция защищена nonReentrant-guard "
                          "(require(!flag); flag=true) [guarded]";
                } else {
                    f.severity = "HIGH";
                    f.message =
                        "внешний вызов происходит до записи в storage (слот "
                        + std::to_string(slot)
                        + "): нарушение checks-effects-interactions, возможна "
                          "повторная входимость (reentrancy)";
                }
                out.push_back(std::move(f));
                fnFlagged = true;
                break;  // одной находки на внешний вызов достаточно
            }
        }

        // непрозрачная запись в storage-агрегат (поле struct / элемент массива) ПОСЛЕ
        // внешнего вызова: модель не видит поле, но паттерн подозрителен. честное LOW-
        // предупреждение вместо тихого ложного негатива (только если HIGH ещё не найден)
        if (!fnFlagged) {
            for (const auto* call : calls) {
                int guardSlot = -1;
                if (isGuardedCall(fn, call, guardSlot, DT)) continue;   // под guard — не пугаем
                bool warned = false;
                for (const auto* op : opaqueStores) {
                    if (!instrReachableAfterCall(call, op)) continue;
                    report::Finding f;
                    f.rule     = "reentrancy";
                    f.severity = "LOW";
                    f.function = fn.getName().str();
                    if (const llvm::DebugLoc& dl = op->getDebugLoc()) f.line = dl.getLine();
                    f.message =
                        "непрозрачная запись в storage-агрегат (поле struct/элемент массива) "
                        "после внешнего вызова: модель не различает поле — ВОЗМОЖНА reentrancy, "
                        "проверьте вручную (не подтверждено)";
                    out.push_back(std::move(f));
                    warned = true;
                    break;
                }
                if (warned) break;
            }
        }

        // cross-contract reentrancy (эвристика, MEDIUM): ETH-value-отправляющий внешний
        // вызов U (классический недоверенный триггер, возможен callback), после которого
        // достижим ДРУГОЙ внешний state-changing вызов E в другой контракт (напр.
        // ledger.debit()). во время callback из U контракт E ещё не обновлён →
        // cross-contract reentrancy (кейс Lendf.Me). честная граница: это ФОРМА, не
        // подтверждённый inter-procedural эффект (полный foundation — проход 2b)
        if (!fnFlagged) {
            for (const auto* u : calls) {
                if (!u->getCalledFunction() ||
                    u->getCalledFunction()->getName() != "scsa.external_call" ||
                    u->arg_size() < 2)
                    continue;
                auto* uv = llvm::dyn_cast<llvm::ConstantInt>(u->getArgOperand(1));
                if (uv && uv->isZero()) continue;   // U должен слать ETH-value
                bool uToSender = trace::reachesCall(u->getArgOperand(0), "evm.msg_sender");
                for (const auto* e : calls) {
                    if (e == u || !e->getCalledFunction() ||
                        e->getCalledFunction()->getName() != "scsa.external_call")
                        continue;
                    if (!instrReachableAfterCall(u, e)) continue;   // E достижим ПОСЛЕ U
                    // E — в ДРУГОЙ контракт (не тот же msg.sender, что U): иначе это тот же
                    // внешний адресат, а не кросс-контрактный эффект
                    bool eToSender = trace::reachesCall(e->getArgOperand(0), "evm.msg_sender");
                    if (uToSender && eToSender) continue;
                    // подтверждён ли эффект: E резолвлен в имплементера, чей метод реально
                    // пишет состояние (метка scsa.xcall.effect от lowering, проход 2b)
                    bool confirmed = e->getMetadata("scsa.xcall.effect") != nullptr;
                    report::Finding f;
                    f.rule     = "reentrancy";
                    f.severity = confirmed ? "HIGH" : "MEDIUM";
                    f.function = fn.getName().str();
                    f.line     = findingLine(u, nullptr);
                    f.message = confirmed
                        ? "cross-contract reentrancy (ПОДТВЕРЖДЁН): после ETH-value-вызова "
                          "достижим внешний вызов в ДРУГОЙ контракт, чей метод ПИШЕТ состояние "
                          "— во время callback тот контракт не обновлён (эффект вне текущего; ср. Lendf.Me)"
                        : "cross-contract reentrancy: после ETH-value-вызова достижим внешний "
                          "state-changing вызов в ДРУГОЙ контракт — во время callback тот "
                          "контракт ещё не обновлён (эффект вне текущего контракта; ср. Lendf.Me)";
                    out.push_back(std::move(f));
                    fnFlagged = true;
                    break;
                }
                if (fnFlagged) break;
            }
        }

        // cross-function reentrancy: функция локально CEI-чистая (findFlagged==false),
        // но value внешнего вызова вычислен из storage-слота S, который ЭТА функция
        // НЕ обнуляет, а обнуляет соседняя. Атакующий из callback повторно входит,
        // пока S не обновлён (drain). см. отчёт ревью (cross_function/read-only).
        if (!fnFlagged) {
            for (const auto* call : calls) {
                // только внешние вызовы имеют сигнатуру (i160 target, i256 value)
                if (!call->getCalledFunction() ||
                    call->getCalledFunction()->getName() != "scsa.external_call") continue;
                int s = -1;
                // глубокая трасса: value часто вычислен как balance*price/1e18
                if (!trace::reachesStorageLoadDeep(call->getArgOperand(1), s) || s < 0) continue;
                bool fWritesS   = writers.count(s) && writers[s].count(&fn);
                bool anyWritesS = writers.count(s) && !writers[s].empty();
                if (fWritesS || !anyWritesS) continue;   // F сам обновляет S, либо никто
                report::Finding f;
                f.rule        = "reentrancy";
                f.severity    = "MEDIUM";
                f.function    = fn.getName().str();
                f.storageSlot = s;
                f.line        = findingLine(call, nullptr);
                f.message =
                    "cross-function reentrancy: сумма вызова вычислена из storage "
                    "(слот " + std::to_string(s) + "), который обнуляется в ДРУГОЙ "
                    "функции — атакующий повторно входит до обновления состояния";
                out.push_back(std::move(f));
                break;
            }
        }
    }

    // ---- read-only reentrancy (класс Curve-2023, ~$70M; Slither почти не ловит) ----
    // слот S мутируется ПОСЛЕ value-отправляющего внешнего вызова (в какой-то функции),
    // а view-геттер G (без записей и внешних вызовов, возвращает значение) читает S. во
    // время реентрантного callback G отдаёт УСТАРЕВШЕЕ состояние → внешние интеграторы,
    // читающие G (оракул цены/virtual_price), обманываются. импакт кросс-контрактный,
    // но сама сигнатура видна в одном контракте (честная эвристика, MEDIUM)
    {
        std::unordered_set<int> mutatedAfterValueCall;
        for (const llvm::Function& fn : m) {
            if (fn.isDeclaration()) continue;
            std::vector<const llvm::CallInst*>  vcalls;
            std::vector<const llvm::StoreInst*> sts;
            for (const auto& bb : fn)
                for (const auto& inst : bb) {
                    if (auto* c = asExternalCall(inst)) {
                        // read-only-reentrancy требует ПЕРЕДАЧУ управления атакующему: ETH-value
                        // (Curve-2023) ИЛИ raw low-level `.call`. high-level типизированный вызов
                        // к config'd gauge/router (value=0) control атакующему НЕ отдаёт → иначе
                        // FP на КАЖДОЙ yield-стратегии (Beefy harvest: claim/swap → update accounting)
                        bool ethValue = false;
                        if (c->arg_size() >= 2)
                            if (auto* cv = llvm::dyn_cast<llvm::ConstantInt>(c->getArgOperand(1)))
                                ethValue = !cv->isZero();
                        if (ethValue || c->getMetadata("scsa.call.raw")) vcalls.push_back(c);
                    }
                    if (auto* s = asStorageStore(inst)) sts.push_back(s);
                }
            for (const auto* c : vcalls)
                for (const auto* s : sts)
                    if (storeReachableAfterCall(c, s)) {
                        int sl = storageSlotOf(s);
                        if (sl >= 0) mutatedAfterValueCall.insert(sl);
                    }
        }
        for (const llvm::Function& fn : m) {
            if (fn.isDeclaration() || fn.getReturnType()->isVoidTy()) continue;
            if (fn.getMetadata("scsa.internal")) continue;   // геттер должен быть внешне-читаем
            bool hasStore = false, hasCall = false;
            int readSlot = -1;
            for (const auto& bb : fn)
                for (const auto& inst : bb) {
                    if (asStorageStore(inst)) hasStore = true;
                    if (asExternalCall(inst)) hasCall = true;
                    if (auto* ld = asStorageLoad(inst)) {
                        int sl = storageSlotOf(ld);
                        if (sl >= 0 && mutatedAfterValueCall.count(sl)) readSlot = sl;
                    }
                }
            if (hasStore || hasCall || readSlot < 0) continue;   // не чистый view-геттер мутируемого слота
            report::Finding f;
            f.rule        = "read-only-reentrancy";
            f.severity    = "MEDIUM";
            f.function    = fn.getName().str();
            f.storageSlot = readSlot;
            f.message =
                "read-only reentrancy: view-функция возвращает состояние (слот "
                + std::to_string(readSlot) + "), которое мутируется ПОСЛЕ внешнего вызова "
                "в другой функции — во время реентрантного callback геттер отдаёт устаревшие "
                "данные (внешние интеграторы/оракулы обманываются; ср. Curve 2023)";
            out.push_back(std::move(f));
        }
    }
    return out;
}

}  // namespace solc::analyzer
