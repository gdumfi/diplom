#include "solc/analyzer/PatternDetectors.h"

#include <queue>
#include <unordered_set>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>

#include "solc/analyzer/IRTrace.h"

namespace solc::analyzer {
namespace {

int lineOf(const llvm::Instruction* inst) {
    if (const llvm::DebugLoc& dl = inst->getDebugLoc()) return dl.getLine();
    return -1;
}

// есть ли проверка msg.sender, гейтящая revert и ДОМИНИРУЮЩАЯ target (т.е. на ВСЕХ
// путях к нему). строже, чем hasMsgSenderGate: проверка на несвязанной ветке НЕ
// подавляет находку (был ложный негатив — см. аудит)
bool hasDominatingMsgSenderGate(const llvm::Function& fn, const llvm::Instruction* target,
                                const llvm::DominatorTree& DT) {
    for (const auto& bb : fn)
        for (const auto& inst : bb) {
            const auto* ic = llvm::dyn_cast<llvm::ICmpInst>(&inst);
            if (!ic) continue;
            if (!trace::reachesCall(ic->getOperand(0), "evm.msg_sender") &&
                !trace::reachesCall(ic->getOperand(1), "evm.msg_sender"))
                continue;
            if (!trace::gatesRevert(ic)) continue;
            if (DT.dominates(ic, target)) return true;
        }
    return false;
}

// достижима ли инструкция target ТОЛЬКО через ветку сравнения msg.sender (control-dependent).
// покрывает proxy-паттерн `ifAdmin`: `if (msg.sender == _admin) { delegatecall } else { _fallback() }`
// — delegatecall исполняется лишь при msg.sender==admin → это admin-гейт, target не attacker-controlled.
// шире, чем hasDominatingMsgSenderGate: ловит и if/else-в-fallback (ветка НЕ ревертит), и
// require(msg.sender==admin) (ветка ревертит). признак control-dependence — РОВНО одна из ветвей
// условного перехода по msg.sender-сравнению доминирует блок target
bool guardedByMsgSenderBranch(const llvm::Function& fn, const llvm::Instruction* target,
                              const llvm::DominatorTree& DT) {
    const llvm::BasicBlock* tb = target->getParent();
    for (const auto& bb : fn) {
        const auto* br = llvm::dyn_cast<llvm::BranchInst>(bb.getTerminator());
        if (!br || !br->isConditional()) continue;
        const auto* ic = llvm::dyn_cast<llvm::ICmpInst>(br->getCondition());
        if (!ic) continue;
        if (!trace::reachesCall(ic->getOperand(0), "evm.msg_sender") &&
            !trace::reachesCall(ic->getOperand(1), "evm.msg_sender"))
            continue;
        bool d0 = DT.dominates(br->getSuccessor(0), tb);
        bool d1 = DT.dominates(br->getSuccessor(1), tb);
        if (d0 != d1) return true;   // ровно одна ветка msg.sender-проверки доминирует target
    }
    return false;
}

}  // namespace

report::Findings TxOriginDetector::analyze(const llvm::Module& m) {
    report::Findings out;
    for (const llvm::Function& fn : m) {
        if (fn.isDeclaration()) continue;
        bool flagged = false;
        for (const auto& bb : fn) {
            for (const auto& inst : bb) {
                const auto* ic = llvm::dyn_cast<llvm::ICmpInst>(&inst);
                if (!ic) continue;
                if (!trace::reachesCall(ic->getOperand(0), "evm.tx_origin") &&
                    !trace::reachesCall(ic->getOperand(1), "evm.tx_origin"))
                    continue;
                // только если сравнение реально гейтит revert (это авторизация),
                // а не просто вычисление (напр. bool same = tx.origin == msg.sender)
                if (!trace::gatesRevert(ic)) continue;
                report::Finding f;
                f.rule     = "tx-origin";
                f.severity = "MEDIUM";
                f.function = fn.getName().str();
                f.line     = lineOf(ic);
                f.message  = "tx.origin используется в проверке прав: фишинг-уязвимость "
                             "(SWC-115) — используйте msg.sender";
                out.push_back(std::move(f));
                flagged = true;
                break;
            }
            if (flagged) break;
        }
    }
    return out;
}

report::Findings UncheckedCallDetector::analyze(const llvm::Module& m) {
    report::Findings out;
    for (const llvm::Function& fn : m) {
        if (fn.isDeclaration()) continue;
        for (const auto& bb : fn) {
            for (const auto& inst : bb) {
                const auto* call = llvm::dyn_cast<llvm::CallInst>(&inst);
                if (!call) continue;
                auto* callee = call->getCalledFunction();
                if (!callee || callee->getName() != "scsa.external_call") continue;
                // SWC-104 применим ТОЛЬКО к low-level .call/.delegatecall/.staticcall/.send,
                // где bool success можно молча отбросить (метка scsa.call.raw). высокоуровневые
                // вызовы (token.transfer/safeTransfer/любой interface-метод) авто-реверят при
                // неудаче — проверять нечего. без этого фильтра safeTransfer давал FP на каждом
                // реальном OZ-контракте
                if (!call->getMetadata("scsa.call.raw")) continue;
                // результат (флаг успеха) никак не используется -> не проверяется
                if (!call->use_empty()) continue;
                report::Finding f;
                f.rule     = "unchecked-call";
                f.severity = "LOW";
                f.function = fn.getName().str();
                f.line     = lineOf(call);
                f.message  = "результат внешнего вызова не проверяется (SWC-104): "
                             "сбой вызова останется незамеченным";
                out.push_back(std::move(f));
            }
        }
    }
    return out;
}

report::Findings SelfdestructDetector::analyze(const llvm::Module& m) {
    report::Findings out;
    // OZ-абстракция: _msgSender() (Context) и auth-гварды типа _checkOwner (Ownable) —
    // проверка прав спрятана за internal-вызовом. распознаём их, чтобы не давать FP на
    // `destroy() onlyOwner` (проверено на 1inch mainnet)
    auto msgSenderFns = trace::collectMsgSenderFns(m);
    std::unordered_set<const llvm::Function*> authGuards;
    for (const llvm::Function& fn : m)
        if (!fn.isDeclaration() && trace::isAuthGuardFn(fn, msgSenderFns)) authGuards.insert(&fn);

    for (const llvm::Function& fn : m) {
        if (fn.isDeclaration()) continue;
        if (fn.getMetadata("scsa.unmodeled_modifier")) continue;  // возможна защита в базовом модификаторе
        const llvm::CallInst* sd = nullptr;
        for (const auto& bb : fn)
            for (const auto& inst : bb)
                if (auto* c = llvm::dyn_cast<llvm::CallInst>(&inst))
                    if (c->getCalledFunction() &&
                        c->getCalledFunction()->getName() == "evm.selfdestruct") { sd = c; break; }
        if (!sd) continue;
        // проверка прав должна ДОМИНИРОВАТЬ selfdestruct (на всех путях к нему), иначе
        // msg.sender-гейт на несвязанной ветке ложно подавлял бы находку (FN, см. аудит)
        llvm::DominatorTree DT(const_cast<llvm::Function&>(fn));
        if (hasDominatingMsgSenderGate(fn, sd, DT)) continue;   // свой msg.sender-гейт
        // защита через вызов auth-гвард-функции (напр. onlyOwner→_checkOwner()), доминирующий sd
        bool guardedByCall = false;
        for (const auto& bb : fn)
            for (const auto& inst : bb)
                if (auto* c = llvm::dyn_cast<llvm::CallInst>(&inst))
                    if (auto* f = c->getCalledFunction())
                        if (authGuards.count(f) && DT.dominates(c, sd)) guardedByCall = true;
        if (guardedByCall) continue;
        report::Finding f;
        f.rule     = "selfdestruct";
        f.severity = "HIGH";
        f.function = fn.getName().str();
        f.line     = lineOf(sd);
        f.message  = "неограниченный selfdestruct: любой может уничтожить контракт "
                     "(SWC-106) — добавьте проверку прав";
        out.push_back(std::move(f));
    }
    return out;
}

report::Findings DelegatecallDetector::analyze(const llvm::Module& m) {
    report::Findings out;
    for (const llvm::Function& fn : m) {
        if (fn.isDeclaration()) continue;
        for (const auto& bb : fn)
            for (const auto& inst : bb) {
                const auto* call = llvm::dyn_cast<llvm::CallInst>(&inst);
                if (!call || !call->getCalledFunction() ||
                    call->getCalledFunction()->getName() != "scsa.external_call") continue;
                auto* md = call->getMetadata("scsa.call.kind");
                if (!md || md->getNumOperands() != 1) continue;
                auto* ks = llvm::dyn_cast<llvm::MDString>(md->getOperand(0));
                if (!ks || ks->getString() != "delegatecall") continue;
                // target = 1-й аргумент; константный адрес считаем доверенным
                if (llvm::isa<llvm::ConstantInt>(call->getArgOperand(0))) continue;
                // simulate-функция: всегда ревертит (нет ни одного ret) → эффекты
                // delegatecall не сохраняются → не эксплуатируемо (1inch simulate, FP)
                bool hasReturn = false;
                for (const auto& b2 : fn)
                    if (llvm::isa<llvm::ReturnInst>(b2.getTerminator())) { hasReturn = true; break; }
                if (!hasReturn) continue;
                // admin-гейт: delegatecall под проверкой msg.sender==admin (proxy `ifAdmin`
                // upgradeToAndCall) или require(msg.sender==admin) — target не attacker-controlled,
                // а доверенная имплементация от админа (стандартный upgrade-паттерн, не баг)
                llvm::DominatorTree DT(const_cast<llvm::Function&>(fn));
                if (guardedByMsgSenderBranch(fn, call, DT)) continue;
                if (hasDominatingMsgSenderGate(fn, call, DT)) continue;
                report::Finding f;
                f.rule     = "delegatecall";
                f.severity = "HIGH";
                f.function = fn.getName().str();
                f.line     = lineOf(call);
                f.message  = "delegatecall в недоверенный (не константный) адрес: "
                             "исполнение чужого кода над нашим storage (SWC-112)";
                out.push_back(std::move(f));
                break;   // одной находки на функцию достаточно
            }
    }
    return out;
}

namespace {
// вызов @scsa.oracle_read (DEX-spot цена) или nullptr
const llvm::CallInst* asOracleRead(const llvm::Instruction& inst) {
    if (auto* c = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (c->getCalledFunction() && c->getCalledFunction()->getName() == "scsa.oracle_read")
            // Chainlink-чтения — не spot-манипулируемы (отдельный OracleStalenessDetector)
            if (!c->getMetadata("scsa.oracle.chainlink"))
                return c;
    return nullptr;
}

// Chainlink price-feed чтение (маркер scsa.oracle.chainlink)
const llvm::CallInst* asChainlinkRead(const llvm::Instruction& inst) {
    if (auto* c = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (c->getCalledFunction() && c->getCalledFunction()->getName() == "scsa.oracle_read" &&
            c->getMetadata("scsa.oracle.chainlink"))
            return c;
    return nullptr;
}
// доходит ли значение цены форвард-тейнтом (через арифметику/приведения/phi/select) до
// value отправляемого внешнего вызова ИЛИ до записи в storage (баланс) — критический сток
bool oracleReachesSink(const llvm::CallInst* oracle) {
    std::unordered_set<const llvm::Value*> seen;
    std::queue<const llvm::Value*> q;
    q.push(oracle); seen.insert(oracle);
    while (!q.empty()) {
        const llvm::Value* v = q.front(); q.pop();
        for (const llvm::User* u : v->users()) {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(u)) {
                auto* fn = call->getCalledFunction();
                if (fn && fn->getName() == "scsa.external_call" && call->arg_size() >= 2 &&
                    call->getArgOperand(1) == v)
                    return true;                                  // цена → value отправки средств
                continue;                                         // внутрь других вызовов не идём
            }
            if (auto* st = llvm::dyn_cast<llvm::StoreInst>(u)) {
                if (st->getValueOperand() == v && st->getPointerAddressSpace() == 1)
                    return true;                                  // цена → запись баланса (mint)
                continue;
            }
            if (auto* inst = llvm::dyn_cast<llvm::Instruction>(u)) {
                bool prop = llvm::isa<llvm::BinaryOperator>(inst) || llvm::isa<llvm::CastInst>(inst) ||
                            llvm::isa<llvm::PHINode>(inst) || llvm::isa<llvm::SelectInst>(inst);
                if (prop && seen.insert(inst).second) q.push(inst);
            }
        }
    }
    return false;
}
}  // namespace

report::Findings OraclePriceDetector::analyze(const llvm::Module& m) {
    report::Findings out;
    for (const llvm::Function& fn : m) {
        if (fn.isDeclaration()) continue;
        bool flagged = false;
        for (const auto& bb : fn) {
            for (const auto& inst : bb) {
                const auto* oracle = asOracleRead(inst);
                if (!oracle || !oracleReachesSink(oracle)) continue;
                report::Finding f;
                f.rule     = "price-manipulation";
                f.severity = "MEDIUM";
                f.function = fn.getName().str();
                f.line     = lineOf(oracle);
                f.message  = "spot-price манипуляция: значение DEX-spot источника "
                             "(getReserves/slot0/getAmountsOut) идёт в отправку средств/запись "
                             "баланса без TWAP — атакующий флеш-займом искажает цену (SWC-oracle)";
                out.push_back(std::move(f));
                flagged = true;
                break;
            }
            if (flagged) break;
        }
    }
    return out;
}

report::Findings OracleStalenessDetector::analyze(const llvm::Module& m) {
    report::Findings out;
    for (const llvm::Function& fn : m) {
        if (fn.isDeclaration()) continue;
        // сама реализация AggregatorV3Interface (price-feed адаптер/обёртка) forwardит updatedAt
        // потребителю — проверять свежесть внутри неё не обязана. flag только на ПОТРЕБИТЕЛЯХ,
        // иначе FP на каждом wrapper-фиде (TermMaxERC4626PriceFeed.latestRoundData — проверено)
        llvm::StringRef fname = fn.getName();
        if (fname == "latestRoundData" || fname == "getRoundData" ||
            fname == "latestAnswer" || fname == "latestRound")
            continue;
        const llvm::CallInst* clRead = nullptr;   // первое Chainlink-чтение
        bool checksTime = false;                  // функция сверяет block.timestamp?
        for (const auto& bb : fn)
            for (const auto& inst : bb) {
                if (!clRead)
                    if (const auto* c = asChainlinkRead(inst)) clRead = c;
                if (const auto* c = llvm::dyn_cast<llvm::CallInst>(&inst))
                    if (c->getCalledFunction() &&
                        c->getCalledFunction()->getName() == "evm.block_timestamp")
                        checksTime = true;
            }
        if (!clRead || checksTime) continue;      // нет оракула, либо свежесть проверяется
        report::Finding f;
        f.rule     = "oracle-staleness";
        f.severity = "MEDIUM";
        f.function = fn.getName().str();
        f.line     = lineOf(clRead);
        f.message  = "Chainlink-цена используется без проверки свежести: функция читает "
                     "AggregatorV3Interface, но не сверяет updatedAt с block.timestamp — "
                     "устаревшая/залипшая цена принимается как валидная (Yarus-2)";
        out.push_back(std::move(f));
    }
    return out;
}

namespace {

// восходит ли значение к АРГУМЕНТУ функции (attacker-controlled) сквозь приведения
// ширины/типа. отличает target из calldata (опасно) от storage-загрузки (доверенный
// адрес) или msg.sender (@evm.msg_sender — не аргумент, безопасно вызвать себя)
bool reachesArgument(const llvm::Value* v) {
    for (int i = 0; i < 16 && v; ++i) {
        if (llvm::isa<llvm::Argument>(v)) return true;
        const auto* inst = llvm::dyn_cast<llvm::Instruction>(v);
        if (!inst) return false;
        if (llvm::isa<llvm::TruncInst>(inst) || llvm::isa<llvm::ZExtInst>(inst) ||
            llvm::isa<llvm::SExtInst>(inst) || llvm::isa<llvm::IntToPtrInst>(inst) ||
            llvm::isa<llvm::PtrToIntInst>(inst) || llvm::isa<llvm::BitCastInst>(inst))
            v = inst->getOperand(0);
        else
            return false;
    }
    return false;
}

// строковое значение метаданного scsa.call.kind ("call"/"delegatecall") или ""
llvm::StringRef callKind(const llvm::CallInst* c) {
    if (auto* md = c->getMetadata("scsa.call.kind"))
        if (md->getNumOperands() == 1)
            if (auto* ks = llvm::dyn_cast<llvm::MDString>(md->getOperand(0)))
                return ks->getString();
    return {};
}

}  // namespace

report::Findings ArbitraryCallDetector::analyze(const llvm::Module& m) {
    report::Findings out;
    // auth-гварды за internal-вызовом (OZ onlyOwner→_checkOwner→_msgSender) — как в
    // Selfdestruct/AccessControl. функция, вызывающая такой гвард, считается защищённой
    auto msgSenderFns = trace::collectMsgSenderFns(m);
    std::unordered_set<const llvm::Function*> authGuards;
    for (const llvm::Function& fn : m)
        if (!fn.isDeclaration() && trace::isAuthGuardFn(fn, msgSenderFns)) authGuards.insert(&fn);
    auto callsAuthGuard = [&](const llvm::Function& fn) {
        for (const auto& bb : fn)
            for (const auto& inst : bb)
                if (auto* c = llvm::dyn_cast<llvm::CallInst>(&inst))
                    if (auto* f = c->getCalledFunction())
                        if (authGuards.count(f)) return true;
        return false;
    };

    for (const llvm::Function& fn : m) {
        if (fn.isDeclaration()) continue;
        // только внешние точки входа (internal-хелперы вызываются защищённым родителем)
        if (fn.getMetadata("scsa.internal")) continue;
        // защищённая контролем прав функция: target приходит от доверенного вызывающего
        // (admin/owner/role). блэклист (mapping-гейт, не msg.sender) НЕ считается защитой —
        // именно он был единственным гейтом в Seneca ($6M), и его недостаточно
        if (trace::hasMsgSenderGate(fn) || callsAuthGuard(fn)) continue;

        for (const auto& bb : fn) {
            bool flagged = false;
            for (const auto& inst : bb) {
                const auto* call = llvm::dyn_cast<llvm::CallInst>(&inst);
                if (!call || !call->getCalledFunction() ||
                    call->getCalledFunction()->getName() != "scsa.external_call") continue;
                // только НИЗКОУРОВНЕВЫЙ .call(data): calldata attacker-controlled. типизированный
                // метод token.transfer(recv,amt) (фиксированный селектор) — не arbitrary-call
                if (!call->getMetadata("scsa.call.raw")) continue;
                llvm::StringRef kind = callKind(call);
                if (kind == "delegatecall") continue;   // покрыт DelegatecallDetector
                if (kind == "staticcall") continue;     // read-only — не дренит состояние/одобрения
                // arbitrary-call drain — это `.call(data)`: value=0, абуз calldata/одобрений.
                // value≠0 `.call{value:x}` на произвольный адрес — это unprotected withdrawal
                // (класс access-control/arbitraryTransfer, Fix4), не этот детектор → не дублируем
                if (call->arg_size() < 2) continue;
                auto* cv = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(1));
                if (!cv || !cv->isZero()) continue;
                const llvm::Value* target = call->getArgOperand(0);
                // target attacker-controlled (из аргумента), не storage/const/msg.sender
                if (!reachesArgument(target)) continue;
                report::Finding f;
                f.rule     = "arbitrary-call";
                f.severity = "HIGH";
                f.function = fn.getName().str();
                f.line     = lineOf(call);
                f.message  = "внешний вызов на ПРОИЗВОЛЬНЫЙ адрес из аргумента без проверки прав: "
                             "атакующий заставляет контракт вызвать что угодно от своего имени "
                             "(дренаж одобренных контракту токенов через transferFrom) — "
                             "ср. Multichain/Socket/LiFi";
                out.push_back(std::move(f));
                flagged = true;
                break;   // одной находки на функцию достаточно
            }
            if (flagged) break;
        }
    }
    return out;
}

report::Findings WeakRandomnessDetector::analyze(const llvm::Module& m) {
    report::Findings out;
    for (const llvm::Function& fn : m) {
        if (fn.isDeclaration()) continue;
        bool flagged = false;
        for (const auto& bb : fn) {
            for (const auto& inst : bb) {
                // операция «случайного» выбора: остаток/деление (winner = rnd % players)
                unsigned op = inst.getOpcode();
                if (op != llvm::Instruction::URem && op != llvm::Instruction::UDiv &&
                    op != llvm::Instruction::SRem && op != llvm::Instruction::SDiv) continue;
                // хотя бы один операнд восходит к значению блока (block.timestamp/number)
                bool fromBlock = false;
                for (unsigned i = 0; i < inst.getNumOperands() && i < 2; ++i)
                    if (trace::reachesCall(inst.getOperand(i), "evm.block_timestamp") ||
                        trace::reachesCall(inst.getOperand(i), "evm.block_number"))
                        fromBlock = true;
                if (!fromBlock) continue;
                report::Finding f;
                f.rule     = "weak-randomness";
                f.severity = "MEDIUM";
                f.function = fn.getName().str();
                f.line     = lineOf(&inst);
                f.message  = "случайность из значения блока (block.timestamp/number) в операции "
                             "выбора (%,/): предсказуема и влияема майнером (SWC-120) — "
                             "используйте commit-reveal или VRF";
                out.push_back(std::move(f));
                flagged = true;
                break;
            }
            if (flagged) break;
        }
    }
    return out;
}

report::Findings SlippageDetector::analyze(const llvm::Module& m) {
    report::Findings out;
    for (const llvm::Function& fn : m) {
        if (fn.isDeclaration()) continue;
        const llvm::CallInst* swap = nullptr;   // первый DEX-своп (named-роутер ИЛИ @scsa.swap-маркер)
        bool hasArgCmp = false;                 // сравнение против calldata-параметра (min-out guard)
        bool guardsSender = false;              // проверка msg.sender (access-control → keeper)
        for (const auto& bb : fn)
            for (const auto& inst : bb) {
                if (!swap)
                    if (const auto* c = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                        const auto* cf = c->getCalledFunction();
                        if (c->getMetadata("scsa.swap") || (cf && cf->getName() == "scsa.swap"))
                            swap = c;
                    }
                if (const auto* ic = llvm::dyn_cast<llvm::ICmpInst>(&inst)) {
                    if (reachesArgument(ic->getOperand(0)) || reachesArgument(ic->getOperand(1)))
                        hasArgCmp = true;       // require(out >= minOut) — minOut из calldata
                    if (trace::reachesCall(ic->getOperand(0), "evm.msg_sender") ||
                        trace::reachesCall(ic->getOperand(1), "evm.msg_sender"))
                        guardsSender = true;    // require(msg.sender == manager) — доверенный keeper
                }
            }
        if (!swap) continue;
        // harvest/reinvest/compound свопит средства КОНТРАКТА (не вызывающего). если функция
        // permissionless (нет msg.sender-guard), любой ставит minOut=0 и сэндвичит reward-своп →
        // caller-minOut тут НЕ защита (в отличие от user-свопа своих средств). систематический
        // класс yield-адаптеров (super-vaults: ~10 адаптеров, pkqs90 «no correct oracle slippage»)
        llvm::StringRef fname = fn.getName();
        bool harvestLike = fname.contains("harvest") || fname.contains("Harvest") ||
                           fname.contains("reinvest") || fname.contains("Reinvest") ||
                           fname.contains("compound") || fname.contains("Compound");
        bool permissionlessHarvest = harvestLike && !guardsSender;
        // (а) named-своп без сравнения выхода с параметром (буквально нет min-out); ИЛИ
        // (б) permissionless harvest со свопом (caller-контролируемый / отсутствующий min-out)
        if (hasArgCmp && !permissionlessHarvest) continue;
        report::Finding f;
        f.rule     = "missing-slippage";
        f.severity = "MEDIUM";
        f.function = fn.getName().str();
        f.line     = lineOf(swap);
        f.message  = permissionlessHarvest
            ? "permissionless harvest/reinvest со свопом средств контракта: вызывающий ставит "
              "min-out=0 и сэндвичит reward-своп → MEV-кража наград (Yarus-2). нужен oracle-minOut "
              "или доверенный keeper (не caller-параметр)"
            : "внешний DEX-своп без видимого slippage-guard (сравнения выхода с min-out параметром "
              "из calldata): выход MEV-сэндвича не ограничен (Yarus-2, кандидат — проверь minOut)";
        out.push_back(std::move(f));
    }
    return out;
}

}  // namespace solc::analyzer
