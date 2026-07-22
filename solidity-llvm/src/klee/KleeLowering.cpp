#include "solc/klee/KleeLowering.h"

#include <vector>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Transforms/Utils/Cloning.h>

namespace solc::klee {

namespace {

// адрес-пространство storage (совпадает с codegen kStorageAS)
constexpr unsigned kStorageAS = 1;

// имя вызываемой функции у инструкции-вызова (пусто, если это не прямой вызов)
llvm::StringRef calleeName(const llvm::CallInst* call) {
    const llvm::Function* f = call->getCalledFunction();
    return f ? f->getName() : llvm::StringRef();
}

// если ptr — константа inttoptr(<const> to ptr addrspace(1)), вернуть слот; иначе -1.
// частный случай: inttoptr(0) LLVM сворачивает в null-указатель — это слот 0
long constSlotOfPtr(const llvm::Value* ptr) {
    if (const auto* pn = llvm::dyn_cast<llvm::ConstantPointerNull>(ptr))
        return pn->getType()->getAddressSpace() == kStorageAS ? 0 : -1;
    const auto* ce = llvm::dyn_cast<llvm::ConstantExpr>(ptr);
    if (!ce || ce->getOpcode() != llvm::Instruction::IntToPtr) return -1;
    const auto* ci = llvm::dyn_cast<llvm::ConstantInt>(ce->getOperand(0));
    if (!ci) return -1;
    return static_cast<long>(ci->getZExtValue());
}

// максимальный использованный слот storage — по inttoptr-константам (скаляры) и
// первому аргументу @scsa.map_elem (базовый слот mapping). нужен для размера массива
long scanMaxSlot(llvm::Module& m) {
    long maxSlot = 0;
    for (auto& f : m)
        for (auto& bb : f)
            for (auto& inst : bb) {
                if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(&inst))
                    maxSlot = std::max(maxSlot, constSlotOfPtr(ld->getPointerOperand()));
                if (auto* st = llvm::dyn_cast<llvm::StoreInst>(&inst))
                    maxSlot = std::max(maxSlot, constSlotOfPtr(st->getPointerOperand()));
                if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst))
                    if (calleeName(call) == "scsa.map_elem")
                        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(0)))
                            maxSlot = std::max(maxSlot, static_cast<long>(ci->getZExtValue()));
            }
    return maxSlot;
}

}  // namespace

std::unique_ptr<llvm::Module> KleeLowering::lower(const llvm::Module& src,
                                                  llvm::StringRef entryFn,
                                                  int effectSlot, Mode mode,
                                                  int ownerSlot) {
    const bool acMode = (mode == Mode::AccessControl);
    auto m = llvm::CloneModule(src);
    // отладочная информация KLEE не нужна и мешает при переписывании инструкций
    llvm::StripDebugInfo(*m);
    // целевой triple — чтобы KLEE не предупреждал о несовпадении с хостом
    m->setTargetTriple("x86_64-unknown-linux-gnu");
    auto& ctx = m->getContext();
    const llvm::DataLayout& dl = m->getDataLayout();

    auto* i1   = llvm::Type::getInt1Ty(ctx);
    auto* i32  = llvm::Type::getInt32Ty(ctx);
    auto* i64  = llvm::Type::getInt64Ty(ctx);
    auto* i160 = llvm::IntegerType::get(ctx, 160);
    auto* i256 = llvm::IntegerType::get(ctx, 256);
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* ptrTy  = llvm::PointerType::get(ctx, 0);

    llvm::Function* target = m->getFunction(entryFn);
    if (!target || target->isDeclaration()) return nullptr;

    // --- глобальная модель storage: [N x i256] в addrspace(1) ---
    long n = scanMaxSlot(*m) + 1;
    auto* storageArrTy = llvm::ArrayType::get(i256, n);
    auto* storageG = new llvm::GlobalVariable(
        *m, storageArrTy, /*isConstant=*/false, llvm::GlobalValue::InternalLinkage,
        llvm::ConstantAggregateZero::get(storageArrTy), "scsa.storage", nullptr,
        llvm::GlobalValue::NotThreadLocal, kStorageAS);

    // счётчик глубины реентрантности — ограничивает fork callback (§6, глубина 1),
    // иначе символьное исполнение зациклится на вложенных вызовах
    auto* depthG = new llvm::GlobalVariable(
        *m, i32, false, llvm::GlobalValue::InternalLinkage,
        llvm::ConstantInt::get(i32, 0), "scsa.reentry_depth");

    // сумма отправленного наружу (value всех external_call) — для оракула кражи:
    // при reentrancy одна и та же сумма уходит дважды, а баланс списывается один раз
    auto* totalSentG = new llvm::GlobalVariable(
        *m, i256, false, llvm::GlobalValue::InternalLinkage,
        llvm::ConstantInt::get(i256, 0), "scsa.total_sent");

    // флаг «привилегированный слот записан» — для access-control оракула (ставится в 1
    // при каждой записи в privSlot=effectSlot). после вызова оракул проверяет, что запись
    // достижима при msg.sender != owner (отсутствие контроля доступа)
    auto* privWrittenG = new llvm::GlobalVariable(
        *m, i1, false, llvm::GlobalValue::InternalLinkage,
        llvm::ConstantInt::get(i1, 0), "scsa.priv_written");

    // --- символьные глобали контекста EVM ---
    auto mkSym = [&](llvm::Type* t, const char* name) {
        return new llvm::GlobalVariable(*m, t, false, llvm::GlobalValue::InternalLinkage,
                                        llvm::Constant::getNullValue(t), name);
    };
    llvm::GlobalVariable* gSender = mkSym(i160, "sym.msg_sender");
    llvm::GlobalVariable* gValue  = mkSym(i256, "sym.msg_value");
    llvm::GlobalVariable* gOrigin = mkSym(i160, "sym.tx_origin");
    llvm::GlobalVariable* gTs     = mkSym(i256, "sym.block_timestamp");
    llvm::GlobalVariable* gBlockNum = mkSym(i256, "sym.block_number");

    // --- объявления рантайма KLEE ---
    auto* makeSymTy = llvm::FunctionType::get(voidTy, {ptrTy, i64, ptrTy}, false);
    auto makeSymbolic = m->getOrInsertFunction("klee_make_symbolic", makeSymTy);
    auto* silentExitTy = llvm::FunctionType::get(voidTy, {i32}, false);
    auto silentExit = m->getOrInsertFunction("klee_silent_exit", silentExitTy);
    // klee_report_error(file, line, message, suffix) — сообщить об ошибке (найден баг)
    auto* reportErrTy = llvm::FunctionType::get(
        voidTy, {ptrTy, i32, ptrTy, ptrTy}, false);
    auto reportError = m->getOrInsertFunction("klee_report_error", reportErrTy);
    // klee_assume(cond) — ограничить символьное пространство (условие считается истинным)
    auto* assumeTy = llvm::FunctionType::get(voidTy, {i64}, false);
    auto kleeAssume = m->getOrInsertFunction("klee_assume", assumeTy);

    auto* i8ptr = llvm::PointerType::get(ctx, 0);

    // GEP на storage[slot] в addrspace(1)
    auto storageElemPtr = [&](llvm::IRBuilder<>& b, llvm::Value* slot) -> llvm::Value* {
        auto* idx = b.CreateZExtOrTrunc(slot, i64);
        return b.CreateInBoundsGEP(storageArrTy, storageG,
                                   {llvm::ConstantInt::get(i64, 0), idx});
    };

    // klee_make_symbolic(addr, sizeof(t), "name") — сделать память символьной
    auto symbolizeInto = [&](llvm::IRBuilder<>& b, llvm::Value* addr,
                             llvm::Type* t, const std::string& name) {
        auto* nameG = b.CreateGlobalString(name);
        b.CreateCall(makeSymbolic,
                     {b.CreateBitCast(addr, i8ptr),
                      llvm::ConstantInt::get(i64, dl.getTypeAllocSize(t)),
                      b.CreateBitCast(nameG, i8ptr)});
    };

    // символьные аргументы для вызова функции fn (каждый через alloca+make_symbolic)
    auto symbolicArgsFor = [&](llvm::IRBuilder<>& b, llvm::Function* fn,
                               const std::string& tag) {
        std::vector<llvm::Value*> args;
        unsigned ai = 0;
        for (auto& arg : fn->args()) {
            auto* slot = b.CreateAlloca(arg.getType());
            symbolizeInto(b, slot, arg.getType(), tag + std::to_string(ai++));
            args.push_back(b.CreateLoad(arg.getType(), slot));
        }
        return args;
    };

    // --- переписываем инструкции: собираем worklist, чтобы не портить итерацию ---
    std::vector<llvm::Instruction*> toErase;
    std::vector<llvm::CallInst*>    extCalls;   // точки external_call — под fork-пасс
    std::vector<llvm::StoreInst*>   privStores; // записи в privSlot — под флаг (access-control)
    for (auto& f : *m) {
        if (f.isDeclaration()) continue;
        for (auto& bb : f) {
            for (auto& inst : bb) {
                // storage-указатель из константы inttoptr в операнде load/store -> GEP
                auto lowerPtrOperand = [&](llvm::Instruction* memInst, unsigned opIdx) {
                    long slot = constSlotOfPtr(memInst->getOperand(opIdx));
                    if (slot < 0) return;
                    llvm::IRBuilder<> b(memInst);
                    memInst->setOperand(opIdx, storageElemPtr(
                        b, llvm::ConstantInt::get(i256, slot)));
                };
                if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(&inst))
                    lowerPtrOperand(ld, ld->getPointerOperandIndex());
                if (auto* st = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
                    long slot = constSlotOfPtr(st->getPointerOperand());
                    lowerPtrOperand(st, st->getPointerOperandIndex());
                    // запись в привилегированный слот (access-control) — отметим флагом
                    if (acMode && slot >= 0 && slot == effectSlot) privStores.push_back(st);
                }

                auto* call = llvm::dyn_cast<llvm::CallInst>(&inst);
                if (!call) continue;
                llvm::StringRef name = calleeName(call);
                llvm::IRBuilder<> b(call);

                if (name == "evm.msg_sender") {
                    call->replaceAllUsesWith(b.CreateLoad(i160, gSender, "msg.sender"));
                    toErase.push_back(call);
                } else if (name == "evm.msg_value") {
                    call->replaceAllUsesWith(b.CreateLoad(i256, gValue, "msg.value"));
                    toErase.push_back(call);
                } else if (name == "evm.tx_origin") {
                    call->replaceAllUsesWith(b.CreateLoad(i160, gOrigin, "tx.origin"));
                    toErase.push_back(call);
                } else if (name == "evm.block_timestamp") {
                    call->replaceAllUsesWith(b.CreateLoad(i256, gTs, "now"));
                    toErase.push_back(call);
                } else if (name == "evm.block_number") {
                    call->replaceAllUsesWith(b.CreateLoad(i256, gBlockNum, "block.number"));
                    toErase.push_back(call);
                } else if (name == "evm.revert") {
                    // откат транзакции -> путь для KLEE тупиковый, молча выходим
                    b.CreateCall(silentExit, {llvm::ConstantInt::get(i32, 0)});
                    toErase.push_back(call);
                } else if (name == "scsa.map_elem") {
                    // элемент mapping: для single-attacker модели ключ опускаем,
                    // коллапсируем к ячейке storage[baseSlot] (тот же атакующий вернётся)
                    auto* gep = storageElemPtr(b, call->getArgOperand(0));
                    call->replaceAllUsesWith(gep);
                    toErase.push_back(call);
                } else if (name == "scsa.external_call") {
                    if (acMode) {
                        // access-control: reentrancy-fork не нужен — заглушка «успех»
                        call->replaceAllUsesWith(llvm::ConstantInt::get(i1, 1));
                        toErase.push_back(call);
                    } else {
                        // fork реентрантного callback обрабатываем отдельным пассом ниже
                        // (нужно расщепление блока — нельзя во время итерации)
                        extCalls.push_back(call);
                    }
                } else if (name == "scsa.opaque_store") {
                    toErase.push_back(call);  // маркер непрозрачной записи — no-op для KLEE
                } else if (name == "evm.selfdestruct") {
                    toErase.push_back(call);  // для символьного исполнения — no-op
                }
            }
        }
    }
    for (auto* inst : toErase) inst->eraseFromParent();

    // access-control: после каждой записи в привилегированный слот ставим флаг priv_written=1
    for (auto* st : privStores) {
        llvm::IRBuilder<> ab(st);
        if (auto* nxt = st->getNextNode()) ab.SetInsertPoint(nxt);
        ab.CreateStore(llvm::ConstantInt::get(i1, 1), privWrittenG);
    }

    // --- fork реентрантного callback глубины 1 (§6.2, ядро метода) ---
    // в точке external_call символьно ветвимся: мир A (вернулись нормально) и мир B
    // (атакующий из callback снова зовёт уязвимую функцию target). если storage ещё не
    // обновлён (эффект после вызова) — мир B выведет средства повторно. глубину
    // ограничивает счётчик scsa.reentry_depth, чтобы KLEE не зациклился
    for (auto* call : extCalls) {
        // учёт отправленного: total_sent += value (2-й аргумент external_call).
        // при реентрантном повторе этот же код исполнится снова → сумма удвоится
        {
            llvm::IRBuilder<> ab(call);
            auto* value = call->getArgOperand(1);
            auto* cur = ab.CreateLoad(i256, totalSentG, "sent");
            ab.CreateStore(ab.CreateAdd(cur, value, "sent.add"), totalSentG);
        }
        llvm::BasicBlock* preBB  = call->getParent();
        // всё ПОСЛЕ call уезжает в contBB; call остаётся последним «телом» preBB
        llvm::BasicBlock* contBB = preBB->splitBasicBlock(call->getNextNode(), "extcall.cont");
        preBB->getTerminator()->eraseFromParent();      // авто-br от split — снимаем
        call->replaceAllUsesWith(llvm::ConstantInt::get(i1, 1));  // вызов «успешен»
        call->eraseFromParent();

        llvm::IRBuilder<> b(preBB);
        auto* reenterSlot = b.CreateAlloca(i1);
        symbolizeInto(b, reenterSlot, i1, "reenter");
        auto* reenter = b.CreateLoad(i1, reenterSlot, "reenter");
        auto* depth   = b.CreateLoad(i32, depthG, "depth");
        auto* atTop   = b.CreateICmpEQ(depth, llvm::ConstantInt::get(i32, 0), "attop");
        auto* doReenter = b.CreateAnd(reenter, atTop, "do.reenter");

        auto* reenterBB = llvm::BasicBlock::Create(
            ctx, "extcall.reenter", preBB->getParent(), contBB);
        b.CreateCondBr(doReenter, reenterBB, contBB);

        llvm::IRBuilder<> rb(reenterBB);
        rb.CreateStore(rb.CreateAdd(depth, llvm::ConstantInt::get(i32, 1)), depthG);
        rb.CreateCall(target, symbolicArgsFor(rb, target, "reenter.arg"));
        rb.CreateStore(depth, depthG);                  // восстанавливаем глубину
        rb.CreateBr(contBB);
    }

    // --- харнесс @main: символизируем контекст+аргументы, зовём уязвимую функцию ---
    auto* mainTy = llvm::FunctionType::get(i32, {}, false);
    auto* mainFn = llvm::Function::Create(
        mainTy, llvm::Function::ExternalLinkage, "main", m.get());
    auto* entry = llvm::BasicBlock::Create(ctx, "entry", mainFn);
    llvm::IRBuilder<> b(entry);

    symbolizeInto(b, gSender, i160, "msg_sender");
    symbolizeInto(b, gValue,  i256, "msg_value");
    symbolizeInto(b, gOrigin, i160, "tx_origin");
    symbolizeInto(b, gTs,     i256, "block_timestamp");
    symbolizeInto(b, gBlockNum, i256, "block_number");
    // ограничиваем msg.value тем же порогом 2^128 (защита от переполнения через депозит)
    {
        auto* bnd = llvm::ConstantInt::get(i256, llvm::APInt(256, 1).shl(128));
        auto* mv = b.CreateLoad(i256, gValue, "mv");
        auto* declTy = llvm::FunctionType::get(voidTy, {i64}, false);
        auto ka = m->getOrInsertFunction("klee_assume", declTy);
        b.CreateCall(ka, {b.CreateZExt(b.CreateICmpULT(mv, bnd, "mv.lt"), i64)});
    }
    // storage символьный — иначе require(balance>0) всегда реверт, путь недостижим.
    // символизируем каждую ячейку через as0-alloca (make_symbolic хочет addrspace 0),
    // значение пишем в ячейку storage (addrspace 1) и суммируем в снапшот баланса —
    // это верхняя граница корректно выводимого за одну транзакцию
    // 2^128 — верхняя граница символьных балансов: реальные значения ≪ 2^128 (весь ETH
    // ≈ 2^87 wei). ограничение убирает переполнение i256 при удвоении (total_sent = 2·balance)
    // → оракул `total_sent > balance` остаётся корректным, а witness-модель реалистична
    auto* bound = llvm::ConstantInt::get(i256, llvm::APInt(256, 1).shl(128));
    unsigned nSlots = storageArrTy->getNumElements();
    // effectSlot вне диапазона → откат к сумме всех слотов
    bool useSlot = (effectSlot >= 0) && (static_cast<unsigned>(effectSlot) < nSlots);
    llvm::Value* initialTotal = llvm::ConstantInt::get(i256, 0);
    llvm::Value* ownerSnapshot = nullptr;   // storage[ownerSlot] на входе (access-control)
    for (unsigned i = 0; i < nSlots; ++i) {
        auto* tmp = b.CreateAlloca(i256);
        symbolizeInto(b, tmp, i256, "storage" + std::to_string(i));
        auto* v = b.CreateLoad(i256, tmp, "slot.init");
        auto* inRange = b.CreateZExt(b.CreateICmpULT(v, bound, "bal.lt"), i64);
        b.CreateCall(kleeAssume, {inRange});
        auto* cell = b.CreateInBoundsGEP(
            storageArrTy, storageG,
            {llvm::ConstantInt::get(i64, 0), llvm::ConstantInt::get(i64, i)});
        b.CreateStore(v, cell);
        // порог = слот-баланс, из которого идёт вывод (effectSlot); иначе — сумма всех
        if (!useSlot || static_cast<unsigned>(effectSlot) == i)
            initialTotal = b.CreateAdd(initialTotal, v, "bal.sum");
        if (acMode && ownerSlot >= 0 && static_cast<unsigned>(ownerSlot) == i)
            ownerSnapshot = v;   // owner на входе (до вызова)
    }

    b.CreateCall(target, symbolicArgsFor(b, target, "arg"));

    // оракул: условие срабатывания (bug) зависит от класса уязвимости
    llvm::Value* bug = nullptr;
    const char* errMsg; const char* errSuf;
    if (acMode) {
        // access-control: привилегированная запись достижима при msg.sender != owner.
        // если функция реально проверяет msg.sender==owner, путь к записи требует
        // равенства → priv_written && (ms!=owner) недостижимо → CLEAN (precision gain
        // над CFG-эвристикой, как counter_guard для reentrancy)
        auto* priv = b.CreateLoad(i1, privWrittenG, "priv.written");
        llvm::Value* notOwner;
        if (ownerSnapshot) {
            auto* ms = b.CreateZExt(b.CreateLoad(i160, gSender, "ms"), i256);
            notOwner = b.CreateICmpNE(ms, ownerSnapshot, "not.owner");
        } else {
            notOwner = llvm::ConstantInt::get(i1, 1);  // owner неизвестен — консервативно
        }
        bug = b.CreateAnd(priv, notOwner, "ac.bug");
        errMsg = "access-control: privileged state changed by non-owner";
        errSuf = "access-control";
    } else {
        // оракул кражи: наружу ушло больше исходного баланса-слота → reentrancy достижима.
        // СОУНДНОСТЬ (проверено эмпирически, не дельта-инвариантом): при корректном CEI
        // списание-ДО-вызова ограничивает реентрантную сумму (require видит уменьшенный
        // баланс) → total_sent ≤ snapshot → молчит; при эффекте-ПОСЛЕ-вызова require в
        // реентрантном заходе видит полный баланс → total_sent до 2·B > B → срабатывает.
        // (дельта-инвариант snapshot−final здесь НЕВЕРЕН: в old-Solidity баланс underflow'ит,
        // суммарное списание == total_sent, что маскирует кражу — см. HISTORY 2026-07-11.)
        auto* sent = b.CreateLoad(i256, totalSentG, "total.sent");
        bug = b.CreateICmpUGT(sent, initialTotal, "stolen");
        errMsg = "reentrancy: funds sent exceed contract balance";
        errSuf = "reentrancy";
    }
    auto* errBB = llvm::BasicBlock::Create(ctx, "violation", mainFn);
    auto* okBB  = llvm::BasicBlock::Create(ctx, "ok", mainFn);
    b.CreateCondBr(bug, errBB, okBB);

    llvm::IRBuilder<> eb(errBB);
    auto* file = eb.CreateGlobalString("contract.sol");
    auto* msg  = eb.CreateGlobalString(errMsg);
    auto* suf  = eb.CreateGlobalString(errSuf);
    eb.CreateCall(reportError, {eb.CreateBitCast(file, ptrTy),
                                llvm::ConstantInt::get(i32, 0),
                                eb.CreateBitCast(msg, ptrTy),
                                eb.CreateBitCast(suf, ptrTy)});
    eb.CreateUnreachable();

    llvm::IRBuilder<> ob(okBB);
    ob.CreateRet(llvm::ConstantInt::get(i32, 0));

    // --- удаляем осиротевшие объявления интринсиков (@evm.*/@scsa.*) ---
    std::vector<llvm::Function*> deadDecls;
    for (auto& f : *m)
        if (f.isDeclaration() &&
            (f.getName().starts_with("evm.") || f.getName().starts_with("scsa.")) &&
            f.use_empty())
            deadDecls.push_back(&f);
    for (auto* f : deadDecls) f->eraseFromParent();

    if (llvm::verifyModule(*m, nullptr)) return nullptr;
    return m;
}

}  // namespace solc::klee
