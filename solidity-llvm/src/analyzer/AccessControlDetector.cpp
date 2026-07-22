#include "solc/analyzer/AccessControlDetector.h"

#include <unordered_set>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>

#include "solc/analyzer/IRTrace.h"

namespace solc::analyzer {
namespace {

// guard-модификатор функции: контекст proxy (onlyProxy/notDelegated) или разовая
// инициализация (initializer/reinitializer/onlyInitializing). такие функции НЕ являются
// самостоятельным access-control-багом: доступ гейтит одноразовость либо вызывающий-proxy,
// а не проверка msg.sender. без этого — FP на каждом OZ-upgradeable/UUPS-контракте
bool hasGuardModifier(const llvm::Function& fn) {
    const auto* md = fn.getMetadata("scsa.modifiers");
    if (!md) return false;
    for (const auto& op : md->operands())
        if (const auto* s = llvm::dyn_cast_or_null<llvm::MDString>(op.get())) {
            llvm::StringRef n = s->getString();
            if (n == "initializer" || n == "reinitializer" || n == "onlyInitializing" ||
                n == "onlyProxy" || n == "notDelegated")
                return true;
        }
    return false;
}

using trace::reachesCall;
using trace::reachesStorageLoad;
using trace::storageSlotOf;

// имя Solidity-типа по LLVM-типу параметра (для типизированных аргументов PoC)
std::string solTypeName(const llvm::Type* t) {
    if (t->isIntegerTy(160)) return "address";
    if (t->isIntegerTy(1)) return "bool";
    return "uint256";
}

const llvm::StoreInst* asStorageStore(const llvm::Instruction& inst) {
    if (auto* st = llvm::dyn_cast<llvm::StoreInst>(&inst))
        if (st->getPointerAddressSpace() == 1) return st;
    return nullptr;
}

using trace::gatesRevert;

// внешний вызов с переводом средств произвольному (не msg.sender, не константному)
// адресу — кандидат на unprotected withdrawal (любой выведет средства куда угодно)
const llvm::CallInst* arbitraryTransfer(const llvm::Function& fn) {
    for (const auto& bb : fn)
        for (const auto& inst : bb) {
            auto* call = llvm::dyn_cast<llvm::CallInst>(&inst);
            if (!call || !call->getCalledFunction() ||
                call->getCalledFunction()->getName() != "scsa.external_call") continue;
            // «вывод средств» = отправка ETH-value. value ДОКАЗУЕМО ноль — это ERC20-перевод
            // (token.transfer), staticcall или протокол-вызов: цель (адрес токена/контракта) не
            // msg.sender, но ETH из контракта не уходит → НЕ вывод средств. иначе FP на flashLoan/
            // safeTransfer/safeTransferFrom (Morpho — проверено на mainnet). ERC20-вывод произвольному
            // адресу мы честно НЕ детектим здесь (получатель — аргумент, не виден в external_call)
            if (call->arg_size() >= 2)
                if (auto* cv = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(1)))
                    if (cv->isZero()) continue;
            const llvm::Value* target = call->getArgOperand(0);
            if (llvm::isa<llvm::ConstantInt>(target)) continue;          // константный адрес — доверенный
            if (trace::reachesCall(target, "evm.msg_sender")) continue;  // отправка себе — легитимно
            return call;
        }
    return nullptr;
}

// запись элемента mapping (указатель — результат @scsa.map_elem). такие слоты
// per-user, а не привилегированное admin-состояние — из контроля доступа исключаем
bool isMappingStore(const llvm::StoreInst* st) {
    if (auto* c = llvm::dyn_cast<llvm::CallInst>(st->getPointerOperand())) {
        auto* fn = c->getCalledFunction();
        return fn && fn->getName() == "scsa.map_elem";
    }
    return false;
}

// пишет ли функция mapping[msg.sender] (per-user учёт)? такая функция — deposit/stake/
// depositFloat/… (пользователь кладёт СВОИ средства, растит свой баланс + total-счётчик),
// а НЕ admin-сеттер. её скалярные записи (total*) — счётчики, не привилегии → не флагаем
// access-control. структурно, БЕЗ зависимости от имени (покрывает кастомные имена вроде depositFloat)
bool writesSenderMapping(const llvm::Function& fn) {
    for (const auto& bb : fn)
        for (const auto& inst : bb)
            if (auto* st = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
                if (st->getPointerAddressSpace() != 1) continue;
                auto* me = llvm::dyn_cast<llvm::CallInst>(st->getPointerOperand());
                if (!me || !me->getCalledFunction() ||
                    me->getCalledFunction()->getName() != "scsa.map_elem") continue;
                for (unsigned i = 0; i < me->arg_size(); ++i)
                    if (reachesCall(me->getArgOperand(i), "evm.msg_sender")) return true;
            }
    return false;
}

// есть ли в теле функции проверка вида require(msg.sender == <storage-слот>),
// РЕАЛЬНО гейтящая revert. при успехе ownerSlot = слот, с которым сравнивают
bool hasOwnerCheck(const llvm::Function& fn, int& ownerSlot) {
    for (const auto& bb : fn)
        for (const auto& inst : bb) {
            const auto* ic = llvm::dyn_cast<llvm::ICmpInst>(&inst);
            if (!ic) continue;
            if (ic->getPredicate() != llvm::CmpInst::ICMP_EQ &&
                ic->getPredicate() != llvm::CmpInst::ICMP_NE)
                continue;
            const llvm::Value* a = ic->getOperand(0);
            const llvm::Value* b = ic->getOperand(1);
            int slot = -1;
            bool matches = (reachesCall(a, "evm.msg_sender") && reachesStorageLoad(b, slot)) ||
                           (reachesCall(b, "evm.msg_sender") && reachesStorageLoad(a, slot));
            if (matches && gatesRevert(ic)) {
                ownerSlot = slot;
                return true;
            }
        }
    return false;
}

// восходит ли значение к загрузке mapping[…msg.sender…] (ключ — msg.sender): паттерн
// role/admin-mapping require(admins[msg.sender]). идём назад через icmp/trunc/zext/
// and/or/xor (gatesRevert вперёд эти узлы не проходит)
bool reachesRoleLoad(const llvm::Value* v, std::unordered_set<const llvm::Value*>& seen,
                     int depth = 0) {
    if (!v || depth > 64 || !seen.insert(v).second) return false;
    if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(v)) {
        if (ld->getPointerAddressSpace() != 1) return false;
        auto* me = llvm::dyn_cast<llvm::CallInst>(ld->getPointerOperand());
        if (!me || !me->getCalledFunction() ||
            me->getCalledFunction()->getName() != "scsa.map_elem") return false;
        for (unsigned i = 0; i < me->arg_size(); ++i)
            if (reachesCall(me->getArgOperand(i), "evm.msg_sender")) return true;
        return false;
    }
    const auto* inst = llvm::dyn_cast<llvm::Instruction>(v);
    if (!inst) return false;
    unsigned op = inst->getOpcode();
    bool passthrough = llvm::isa<llvm::ICmpInst>(inst) || llvm::isa<llvm::TruncInst>(inst) ||
                       llvm::isa<llvm::ZExtInst>(inst) || op == llvm::Instruction::And ||
                       op == llvm::Instruction::Or || op == llvm::Instruction::Xor;
    if (!passthrough) return false;
    for (const auto& o : inst->operands())
        if (reachesRoleLoad(o.get(), seen, depth + 1)) return true;
    return false;
}

// есть ли role-based проверка прав: ветка в revert, чьё условие восходит к
// admins[msg.sender] (mapping(address=>bool) операторов/админов). так авторизуются
// современные протоколы вместо единственного owner-слота
bool hasRoleCheck(const llvm::Function& fn) {
    for (const auto& bb : fn) {
        const auto* term = llvm::dyn_cast<llvm::BranchInst>(bb.getTerminator());
        if (!term || !term->isConditional()) continue;
        bool toRevert = false;
        for (unsigned i = 0; i < term->getNumSuccessors(); ++i)
            if (trace::isRevertBlock(term->getSuccessor(i))) toRevert = true;
        if (!toRevert) continue;
        std::unordered_set<const llvm::Value*> seen;
        if (reachesRoleLoad(term->getCondition(), seen)) return true;
    }
    return false;
}

// имя-конвенция инициализатора (OZ Initializable): initialize / reinitialize / __X_init(_unchained)
bool isInitializerName(llvm::StringRef n) {
    return n == "initialize" || n == "initialise" || n.starts_with("reinitialize") ||
           (n.starts_with("__") && (n.ends_with("_init") || n.ends_with("_init_unchained")));
}

// одноразовый init-lock (модификатор `initializer`): storage-СКАЛЯР `_initialized` сравнивается
// с КОНСТАНТОЙ, это гейтит revert, И тот же слот пишется в функции (require(_initialized < 1);
// _initialized = 1). легитимная разовая инициализация owner/config — не access-control-баг.
// проверяем ВМЕСТЕ с именем: unprotected initialize (без модификатора → без init-lock) остаётся
// находкой; nonReentrant/pause-функция (иное имя) не подавляется этим правилом
bool hasInitLock(const llvm::Function& fn) {
    std::unordered_set<int> stored;   // скалярные слоты, ЗАПИСЫВАЕМЫЕ в функции
    for (const auto& bb : fn)
        for (const auto& inst : bb)
            if (auto* st = asStorageStore(inst))
                if (!isMappingStore(st)) { int s = storageSlotOf(st); if (s >= 0) stored.insert(s); }
    for (const auto& bb : fn)
        for (const auto& inst : bb) {
            const auto* ic = llvm::dyn_cast<llvm::ICmpInst>(&inst);
            if (!ic) continue;
            // check-and-set одного скалярного слота: сравнение с константой + запись того же слота.
            // gatesRevert НЕ требуем — OZ-модификатор компилит `||` в `or i256` через zext i1→i256,
            // и узкий обход revert его не проходит; но сам паттерн (сравнил слот с const и пишет
            // его же) однозначно = одноразовый lock. имя-конвенция страхует от ложной генерализации
            int slot = -1;
            bool a = reachesStorageLoad(ic->getOperand(0), slot) &&
                     llvm::isa<llvm::ConstantInt>(ic->getOperand(1));
            bool b = reachesStorageLoad(ic->getOperand(1), slot) &&
                     llvm::isa<llvm::ConstantInt>(ic->getOperand(0));
            if ((a || b) && slot >= 0 && stored.count(slot)) return true;
        }
    return false;
}

}  // namespace

report::Findings AccessControlDetector::analyze(const llvm::Module& m) {
    report::Findings out;

    // OZ-абстракция: onlyOwner→_checkOwner()→_msgSender() — проверка прав за internal-вызовом.
    // распознаём auth-гвард-функции, чтобы вызов такой считался проверкой (0 FP на OZ)
    auto msgSenderFns = trace::collectMsgSenderFns(m);
    std::unordered_set<const llvm::Function*> authGuards;
    // OZ AccessControl: onlyRole → _checkRole → hasRole(_roles[role].members[msg.sender]) — проверка
    // прав через ВЛОЖЕННЫЙ role-mapping за парой internal-вызовов; структурная трасса msg.sender туда
    // не доходит (account передаётся параметром). распознаём КАНОНИЧЕСКИЙ guard OZ по имени: функция
    // _checkRole/_checkOwner/_authorizeUpgrade = auth-guard, вызывающих подхватит callsAuthGuard.
    // без этого — массовый FP на КАЖДОМ role-based контракте (Ethena StakedUSDe.transferAdmin — проверено)
    auto isRoleGuardName = [](llvm::StringRef n) {
        return n.starts_with("_checkRole") || n == "_checkOwner" || n == "_authorizeUpgrade";
    };
    for (const llvm::Function& fn : m)
        if (!fn.isDeclaration() &&
            (trace::isAuthGuardFn(fn, msgSenderFns) || isRoleGuardName(fn.getName())))
            authGuards.insert(&fn);
    auto callsAuthGuard = [&](const llvm::Function& fn) {
        for (const auto& bb : fn)
            for (const auto& inst : bb)
                if (auto* c = llvm::dyn_cast<llvm::CallInst>(&inst))
                    if (auto* f = c->getCalledFunction())
                        if (authGuards.count(f)) return true;
        return false;
    };

    // проход 1: функции с owner-проверкой, слоты owner и привилегированные слоты
    std::unordered_set<const llvm::Function*> checkedFns;
    std::unordered_set<int> privilegedSlots;
    int contractOwnerSlot = -1;   // слот owner контракта (для KLEE-оракула access-control)
    for (const auto& fn : m) {
        if (fn.isDeclaration()) continue;
        if (fn.getMetadata("scsa.unmodeled_modifier")) continue;  // защита в базовом модификаторе
        int ownerSlot = -1;
        bool owner = hasOwnerCheck(fn, ownerSlot);
        bool role  = !owner && hasRoleCheck(fn);   // role/admin-mapping проверка прав
        bool ag    = !owner && !role && callsAuthGuard(fn);   // onlyOwner→_checkOwner() (OZ)
        if (!owner && !role && !ag) continue;
        checkedFns.insert(&fn);
        if (owner && ownerSlot >= 0) { privilegedSlots.insert(ownerSlot); contractOwnerSlot = ownerSlot; }  // сам owner-слот привилегирован
        // скалярное состояние, пишущееся под owner-проверкой, тоже привилегировано
        // (mapping-элементы исключаем — они per-user, а не admin-состояние)
        for (const auto& bb : fn)
            for (const auto& inst : bb)
                if (auto* st = asStorageStore(inst)) {
                    if (isMappingStore(st)) continue;
                    int slot = storageSlotOf(st);
                    if (slot >= 0) privilegedSlots.insert(slot);
                }
    }
    // исключаем ERC20/ERC4626-accounting слоты (user shares/balances/totalSupply): слот,
    // писанный СТАНДАРТНОЙ функцией (transfer/mint/deposit/withdraw/redeem/…) — это учёт
    // ПОЛЬЗОВАТЕЛЕЙ, а не admin-состояние. иначе permissionless vault-энтрипоинты дают массовый
    // ложный access-control (Sommelier/Gearbox deposit/mint/withdraw/redeem, проверено на mainnet)
    static const std::unordered_set<std::string> kAccountingFns = {
        "transfer", "transferFrom", "approve", "mint", "burn", "deposit", "withdraw", "redeem",
        "permit", "increaseAllowance", "decreaseAllowance",
        "_mint", "_burn", "_transfer", "_update", "_deposit", "_withdraw", "_spendAllowance",
        // стейкинг/награды — тоже ПОЛЬЗОВАТЕЛЬСКИЙ учёт (totalStaked/rewardPerToken), не admin-привилегия.
        // permissionless stake/unstake/claim пишут эти счётчики → без исключения массовый FP на
        // staking/farm-контрактах (MagmaStaking.stake — проверено). owner-слот защищён отдельно (slot!=owner)
        "stake", "unstake", "_stake", "_unstake", "claim", "claimReward", "claimRewards",
        "getReward", "_updateReward", "updateReward", "exit", "withdrawAll", "unstakeAll"};
    for (const auto& fn : m) {
        if (fn.isDeclaration() || !kAccountingFns.count(fn.getName().str())) continue;
        for (const auto& bb : fn)
            for (const auto& inst : bb)
                if (auto* st = asStorageStore(inst)) {
                    if (isMappingStore(st)) continue;
                    int slot = storageSlotOf(st);
                    if (slot >= 0 && slot != contractOwnerSlot) privilegedSlots.erase(slot);
                }
    }
    if (privilegedSlots.empty()) return out;   // контракт не демонстрирует привилегий — не гадаем

    // проход 2: функции без owner-проверки, пишущие привилегированный слот
    for (const auto& fn : m) {
        if (fn.isDeclaration() || checkedFns.count(&fn)) continue;
        // access-control только на ВНЕШНИХ точках входа (как reentrancy): internal-хелпер под
        // guarded-энтрипоинтом (Gearbox _setQuotaRevenue/_pause, Sommelier _disableInitializers)
        // — не самостоятельная уязвимость → иначе FP
        if (fn.getMetadata("scsa.internal")) continue;
        if (fn.getMetadata("scsa.unmodeled_modifier")) continue;  // защита в базовом модификаторе
        if (hasGuardModifier(fn)) continue;  // initializer/onlyProxy-guarded — не access-control-баг
        // старый конструктор (имя == имени контракта) легитимно инициализирует owner
        if (fn.getName() == m.getName()) continue;
        // OZ initializer-guarded initialize(): разовая легитимная инициализация owner/config.
        // требуем И имя-конвенцию, И структурный init-lock — unprotected initialize (без
        // модификатора) остаётся находкой (Fix2, Sommelier initialize FP — проверено на mainnet)
        if (isInitializerName(fn.getName()) && hasInitLock(fn)) continue;
        // (а) запись привилегированного слота без проверки
        const llvm::StoreInst* privWrite = nullptr;
        int privSlot = -1;
        for (const auto& bb : fn) {
            for (const auto& inst : bb) {
                auto* st = asStorageStore(inst);
                if (!st || isMappingStore(st)) continue;
                int slot = storageSlotOf(st);
                if (slot >= 0 && privilegedSlots.count(slot)) { privWrite = st; privSlot = slot; break; }
            }
            if (privWrite) break;
        }
        if (privWrite && !writesSenderMapping(fn)) {
            report::Finding f;
            f.rule = "access-control"; f.severity = "HIGH";
            f.function = fn.getName().str(); f.storageSlot = privSlot;
            f.ownerSlot = contractOwnerSlot;
            // строка привилегированной записи — для триажа (раньше AC-находки шли без строки)
            if (const llvm::DebugLoc& dl = privWrite->getDebugLoc()) f.line = dl.getLine();
            // типы параметров — для типизированных аргументов PoC (напр. setOwner(address)).
            // реальные ABI-типы из !scsa.abi (верный селектор), fallback на llvm-тип
            f.vulnParamCount = static_cast<int>(fn.arg_size());
            if (auto abi = trace::functionParamAbiTypes(fn); !abi.empty())
                f.vulnParamTypes = abi;
            else
                for (const auto& arg : fn.args())
                    f.vulnParamTypes.push_back(solTypeName(arg.getType()));
            f.message = "функция меняет привилегированное состояние (слот "
                        + std::to_string(privSlot) + ") без проверки прав "
                        "(в контракте есть проверка msg.sender==owner / admins[msg.sender] "
                        "в других функциях, здесь её нет): отсутствие контроля доступа";
            out.push_back(std::move(f));
            continue;
        }
        // (б) unprotected withdrawal: вывод средств произвольному адресу без проверки
        if (const auto* c = arbitraryTransfer(fn)) {
            report::Finding f;
            f.rule = "access-control"; f.severity = "HIGH";
            f.function = fn.getName().str();
            if (const llvm::DebugLoc& dl = c->getDebugLoc()) f.line = dl.getLine();
            f.message = "неограниченный вывод средств: внешний перевод на произвольный "
                        "адрес без проверки прав (кто угодно может опустошить контракт)";
            out.push_back(std::move(f));
        }
    }
    return out;
}

}  // namespace solc::analyzer
