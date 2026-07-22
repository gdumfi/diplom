#include "solc/codegen/CodeGen.h"

#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/ErrorHandling.h>

namespace solc::codegen {

CodeGen::CodeGen(DiagnosticEngine& diag) : diag_(diag) {}

// укоренён ли lvalue-агрегат в storage (state var)? для struct-поля/элемента массива
// база рекурсивно: Ident-в-stateVarSlots_ / Member.base / Index.base. локали — false.
// (sema не резолвит базу Member → resolvedKind ненадёжен, проверяем по имени state var)
bool CodeGen::rootsAtStorage(const ast::Expr& e) const {
    using namespace ast;
    switch (e.kind) {
        case ExprKind::Ident:
            return stateVarSlots_.count(static_cast<const IdentExpr&>(e).name) > 0;
        case ExprKind::Member: return rootsAtStorage(*static_cast<const MemberExpr&>(e).base);
        case ExprKind::Index:  return rootsAtStorage(*static_cast<const IndexExpr&>(e).base);
        default:               return false;
    }
}

llvm::Type* CodeGen::llvmType(ast::TypeKind k) {
    switch (k) {
        case ast::TypeKind::Uint256: return llvm::IntegerType::get(*ctx_, 256);
        case ast::TypeKind::Bool:    return llvm::IntegerType::get(*ctx_, 1);
        case ast::TypeKind::Address: return llvm::IntegerType::get(*ctx_, 160);
        case ast::TypeKind::Mapping:
            // mapping не имеет скалярного значения — доступ только через map_elem
            llvm_unreachable("mapping has no scalar llvm type");
    }
    llvm_unreachable("bad TypeKind");
}

llvm::Function* CodeGen::emitFunctionProto(const ast::FunctionDecl& fn) {
    std::vector<llvm::Type*> paramTys;
    for (const auto& p : fn.params) paramTys.push_back(llvmType(p.type.kind));

    llvm::Type* retTy = fn.returnType
        ? llvmType(fn.returnType->kind)
        : llvm::Type::getVoidTy(*ctx_);

    auto* ft = llvm::FunctionType::get(retTy, paramTys, false);
    return llvm::Function::Create(
        ft, llvm::Function::ExternalLinkage, fn.name, module_.get());
}

// mutability/visibility-слова, попадающие в fn.modifiers наравне с кастомными
static bool isMutabilityKeyword(const std::string& s) {
    return s == "payable" || s == "view" || s == "pure" || s == "constant" ||
           s == "external" || s == "internal" || s == "private" || s == "public" ||
           s == "virtual" || s == "override";
}

void CodeGen::emitFunctionBody(const ast::FunctionDecl& fn, llvm::Function* func) {
    locals_.clear();
    storageAliases_.clear();
    currentFn_ = func;

    // если применён кастомный модификатор, которого нет в контракте (вероятно, из
    // базового контракта — наследование не сливаем), помечаем функцию: детекторы
    // access-control/selfdestruct её пропустят, чтобы не давать ложных срабатываний
    for (const auto& mn : fn.modifiers)
        if (!modifierMap_.count(mn) && !isMutabilityKeyword(mn))
            func->setMetadata("scsa.unmodeled_modifier", llvm::MDNode::get(*ctx_, {}));

    // DISubprogram для функции: пустой тип-сигнатура достаточно для номеров строк.
    // DISPFlagDefinition — у функции есть тело; линия — начало объявления функции
    auto* subTy = dib_->createSubroutineType(dib_->getOrCreateTypeArray({}));
    diSP_ = dib_->createFunction(
        diFile_, fn.name, /*LinkageName=*/fn.name, diFile_, fn.loc.line, subTy,
        /*ScopeLine=*/fn.loc.line, llvm::DINode::FlagZero,
        llvm::DISubprogram::SPFlagDefinition);
    func->setSubprogram(diSP_);

    auto* entry = llvm::BasicBlock::Create(*ctx_, "entry", func);
    builder_->SetInsertPoint(entry);

    // пролог (alloca/store параметров) относим к строке объявления функции
    setDebugLoc(fn.loc);

    // alloca + store для каждого параметра — единая модель доступа к параметрам/локалам через load/store
    auto argIt = func->arg_begin();
    for (const auto& p : fn.params) {
        auto* ty  = llvmType(p.type.kind);
        auto* alc = builder_->CreateAlloca(ty, nullptr, p.name);
        builder_->CreateStore(&*argIt, alc);
        locals_[p.name] = LocalSlot{alc, ty};
        ++argIt;
    }

    emitWithModifiers(fn, 0);

    // safety net: emitStmt пока заглушка, без неё verify упадёт на отсутствие терминатора.
    // синтетический терминатор относим к строке функции (позиции в исходнике нет)
    setDebugLoc(fn.loc);
    if (!builder_->GetInsertBlock()->getTerminator()) {
        if (func->getReturnType()->isVoidTy()) {
            builder_->CreateRetVoid();
        } else {
            builder_->CreateRet(llvm::Constant::getNullValue(func->getReturnType()));
        }
    }
    currentFn_ = nullptr;
    // сбрасываем область и текущую позицию: вне тела функции !dbg не ставим,
    // иначе инструкции пролога следующей функции унаследуют чужую DISubprogram
    diSP_ = nullptr;
    builder_->SetCurrentDebugLocation(llvm::DebugLoc());
}

// slot -> указатель в addrspace(1). inttoptr константы-слота: указатели storage
// объявлены 256-битными в datalayout, поэтому ширина i256 совпадает без усечения
llvm::Value* CodeGen::storageSlotPtr(int slot) {
    auto* i256 = llvm::IntegerType::get(*ctx_, 256);
    auto* ptrTy = llvm::PointerType::get(*ctx_, kStorageAS);
    auto* slotConst = llvm::ConstantInt::get(i256, slot);
    return builder_->CreateIntToPtr(slotConst, ptrTy, "slot.ptr");
}

// указатель на элемент mapping base[index] в addrspace(1) через @scsa.map_elem.
// базовый слот берём из base (mapping — всегда state var), ключ приводим к i256
llvm::Value* CodeGen::emitMapElemPtr(const ast::IndexExpr& ix) {
    const auto& baseId = static_cast<const ast::IdentExpr&>(*ix.base);
    return emitMapElemPtrRaw(baseId.resolvedSlot, *ix.index);
}

// то же по явным слоту и ключу — используется и для storage-алиасов (var x = map[k])
llvm::Value* CodeGen::emitMapElemPtrRaw(int baseSlot, const ast::Expr& keyExpr) {
    auto* i256 = llvm::IntegerType::get(*ctx_, 256);
    auto* baseSlotC = llvm::ConstantInt::get(i256, baseSlot);
    auto* key = emitExpr(keyExpr);
    if (!key) return nullptr;
    if (!key->getType()->isIntegerTy(256)) key = builder_->CreateZExt(key, i256, "keyext");
    return builder_->CreateCall(intr_.map_elem, {baseSlotC, key}, "map.elem");
}

// метка storage-доступа: !scsa.storage !{i64 slot} — анализатор восстанавливает слот
void CodeGen::tagStorage(llvm::Instruction* inst, int slot) {
    auto* i64 = llvm::IntegerType::get(*ctx_, 64);
    auto* md  = llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64, slot));
    inst->setMetadata("scsa.storage", llvm::MDNode::get(*ctx_, {md}));
}

// метки внешнего вызова: тип (call/delegatecall/staticcall) и доверие к target.
// в MVP любой внешний вызов считаем недоверенным (консервативно, см. §6 havoc)
void CodeGen::tagExternalCall(llvm::Instruction* call, const char* kindStr) {
    auto* kind = llvm::MDString::get(*ctx_, kindStr);
    call->setMetadata("scsa.call.kind", llvm::MDNode::get(*ctx_, {kind}));
    auto* i1 = llvm::IntegerType::get(*ctx_, 1);
    auto* untrusted = llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i1, 0));
    call->setMetadata("scsa.call.trusted", llvm::MDNode::get(*ctx_, {untrusted}));
}

// эмиссия тела функции с инлайнингом применённых модификаторов.
// имена, не совпавшие с определением модификатора (mutability-слова payable/view/…),
// пропускаем. на _; в теле модификатора рекурсивно эмитим следующий уровень (в итоге
// тело самой функции). так IR отражает реальную семантику: require из onlyOwner
// оказывается в IR функции, и это видят все детекторы
void CodeGen::emitWithModifiers(const ast::FunctionDecl& fn, size_t modIdx) {
    while (modIdx < fn.modifiers.size() && !modifierMap_.count(fn.modifiers[modIdx]))
        ++modIdx;
    if (modIdx >= fn.modifiers.size()) {
        for (const auto& sp : fn.body.stmts) emitStmt(*sp);
        return;
    }
    const ast::ModifierDecl* md = modifierMap_.at(fn.modifiers[modIdx]);
    // параметры модификатора: аргументы вызова мы отбрасываем, поэтому связываем
    // параметры нулём — чтобы ссылки на них в теле не падали (imprecise, но без краха)
    for (const auto& p : md->params) {
        if (locals_.count(p.name)) continue;
        auto* ty  = llvmType(p.type.kind);
        auto* alc = builder_->CreateAlloca(ty, nullptr, p.name);
        builder_->CreateStore(llvm::Constant::getNullValue(ty), alc);
        locals_[p.name] = LocalSlot{alc, ty};
    }
    bool placeholderSeen = false;
    for (const auto& sp : md->body.stmts) {
        if (builder_->GetInsertBlock()->getTerminator()) break;
        if (sp->kind == ast::StmtKind::Placeholder) {
            placeholderSeen = true;
            emitWithModifiers(fn, modIdx + 1);
        } else {
            emitStmt(*sp);
        }
    }
    // модификатор без _; — тело функции всё равно эмитим, иначе эффекты (внешние
    // вызовы, записи storage) пропадут из IR → ложный негатив анализатора
    if (!placeholderSeen && !builder_->GetInsertBlock()->getTerminator())
        emitWithModifiers(fn, modIdx + 1);
}

llvm::Value* CodeGen::emitStateLoad(int slot, ast::TypeKind k) {
    auto* i256 = llvm::IntegerType::get(*ctx_, 256);
    auto* ptr  = storageSlotPtr(slot);
    auto* load = builder_->CreateLoad(i256, ptr, "sload");
    tagStorage(load, slot);
    auto* targetTy = llvmType(k);
    if (targetTy->isIntegerTy(256)) return load;
    // bool/address хранятся в слоте как i256 — сужаем при чтении
    return builder_->CreateTrunc(load, targetTy, "trunc");
}

void CodeGen::emitStateStore(int slot, llvm::Value* v, ast::TypeKind /*k*/) {
    auto* i256 = llvm::IntegerType::get(*ctx_, 256);
    llvm::Value* wide = v;
    if (!v->getType()->isIntegerTy(256)) {
        wide = builder_->CreateZExt(v, i256, "zext");
    }
    auto* ptr = storageSlotPtr(slot);
    auto* st  = builder_->CreateStore(wide, ptr);
    tagStorage(st, slot);
}

llvm::Value* CodeGen::emitExpr(const ast::Expr& e) {
    using namespace ast;
    struct Guard { int& d; Guard(int& x) : d(x) { ++d; } ~Guard() { --d; } } g(exprDepth_);
    if (exprDepth_ > kMaxExprDepth) {
        diag_.error(e.loc, "expression too deeply nested");
        return nullptr;
    }
    switch (e.kind) {
        case ExprKind::NumberLit: {
            const auto& n = static_cast<const NumberLit&>(e);
            // hex-литерал 0x... — основание 16 без префикса, иначе десятичный.
            // проверяем, что литерал влезает в 256 бит (иначе молча оборачивался бы)
            bool hex = n.text.size() > 2 && n.text[0] == '0' &&
                       (n.text[1] == 'x' || n.text[1] == 'X');
            llvm::StringRef digits = hex ? llvm::StringRef(n.text).substr(2) : llvm::StringRef(n.text);
            unsigned radix = hex ? 16 : 10;
            if (llvm::APInt::getBitsNeeded(digits, radix) > 256) {
                diag_.error(e.loc, "integer literal exceeds 256 bits");
                return nullptr;
            }
            return llvm::ConstantInt::get(*ctx_, llvm::APInt(256, digits, radix));
        }
        case ExprKind::BoolLit: {
            const auto& b = static_cast<const BoolLit&>(e);
            return llvm::ConstantInt::get(llvm::Type::getInt1Ty(*ctx_), b.value ? 1 : 0);
        }
        case ExprKind::Ident: {
            const auto& id = static_cast<const IdentExpr&>(e);
            if (id.resolvedKind == IdentExpr::Resolution::Param ||
                id.resolvedKind == IdentExpr::Resolution::Local) {
                auto it = locals_.find(id.name);
                if (it == locals_.end()) {
                    diag_.error(id.loc, "internal: local not found in codegen");
                    return nullptr;
                }
                return builder_->CreateLoad(it->second.type, it->second.alloca, id.name);
            }
            if (id.resolvedKind == IdentExpr::Resolution::Builtin) {
                // now / block.timestamp
                return builder_->CreateCall(intr_.block_timestamp, {}, "now");
            }
            if (id.resolvedKind == IdentExpr::Resolution::StateVar) {
                if (!id.resolvedType) {
                    diag_.error(id.loc, "internal: state var without resolved type");
                    return nullptr;
                }
                return emitStateLoad(id.resolvedSlot, id.resolvedType->kind);
            }
            diag_.error(id.loc, "ident codegen not implemented for this kind yet");
            return nullptr;
        }
        case ExprKind::Binary: {
            const auto& b = static_cast<const BinaryExpr&>(e);
            auto* L = emitExpr(*b.lhs);
            auto* R = emitExpr(*b.rhs);
            if (!L || !R) return nullptr;
            switch (b.op) {
                case BinOp::Add:   return builder_->CreateAdd(L, R, "add");
                case BinOp::Sub:   return builder_->CreateSub(L, R, "sub");
                case BinOp::Mul:   return builder_->CreateMul(L, R, "mul");
                // unsigned div/rem — uint256 беззнаковый
                case BinOp::Div:   return builder_->CreateUDiv(L, R, "udiv");
                case BinOp::Mod:   return builder_->CreateURem(L, R, "urem");
                case BinOp::Eq:    return builder_->CreateICmpEQ(L, R, "eq");
                case BinOp::NotEq: return builder_->CreateICmpNE(L, R, "ne");
                case BinOp::Lt:    return builder_->CreateICmpULT(L, R, "ult");
                case BinOp::Gt:    return builder_->CreateICmpUGT(L, R, "ugt");
                case BinOp::LtEq:  return builder_->CreateICmpULE(L, R, "ule");
                case BinOp::GtEq:  return builder_->CreateICmpUGE(L, R, "uge");
                // mvp: && и || без short-circuit — операнды чистые, Sema гарантирует bool
                case BinOp::AndAnd: return builder_->CreateAnd(L, R, "and");
                case BinOp::OrOr:   return builder_->CreateOr(L, R, "or");
            }
            llvm_unreachable("bad BinOp");
        }
        case ExprKind::Unary: {
            const auto& u = static_cast<const UnaryExpr&>(e);
            auto* V = emitExpr(*u.operand);
            if (!V) return nullptr;
            // !x == x ^ 1 для i1
            auto* allOnes = llvm::ConstantInt::get(V->getType(), 1);
            return builder_->CreateXor(V, allOnes, "not");
        }
        case ExprKind::Member: {
            const auto& m = static_cast<const MemberExpr&>(e);
            // msg.sender / msg.value — единственная форма member-access в MVP.
            // base распознаём синтаксически: emitExpr на base звать нельзя,
            // потому что "msg" не настоящий Symbol, и Sema его не разрешает.
            if (m.base->kind == ExprKind::Ident) {
                const auto& base = static_cast<const IdentExpr&>(*m.base);
                if (base.name == "msg") {
                    if (m.member == "sender") {
                        return builder_->CreateCall(intr_.msg_sender, {}, "msg.sender");
                    }
                    if (m.member == "value") {
                        return builder_->CreateCall(intr_.msg_value, {}, "msg.value");
                    }
                }
                if (base.name == "tx" && m.member == "origin") {
                    return builder_->CreateCall(intr_.tx_origin, {}, "tx.origin");
                }
                // блок-контекст: символьные значения для KLEE (§4.3), а не opaque 0 —
                // иначе гарды вида `block.number > lastBlock` схлопываются в ложь
                if (base.name == "block" && m.member == "timestamp") {
                    return builder_->CreateCall(intr_.block_timestamp, {}, "block.timestamp");
                }
                if (base.name == "block" && m.member == "number") {
                    return builder_->CreateCall(intr_.block_number, {}, "block.number");
                }
                // acc.field, где acc — storage-ссылка (var acc = map[key]): чтение
                // ячейки storage. поля не различаем (одна ячейка), для анализа достаточно
                auto ai = storageAliases_.find(base.name);
                if (ai != storageAliases_.end()) {
                    auto* i256 = llvm::IntegerType::get(*ctx_, 256);
                    auto* ptr = emitMapElemPtrRaw(ai->second.baseSlot, *ai->second.key);
                    if (!ptr) return nullptr;
                    auto* load = builder_->CreateLoad(i256, ptr, "mload");
                    tagStorage(load, ai->second.baseSlot);
                    return load;
                }
            }
            // поле struct / .length / прочее — семантику агрегатов не моделируем,
            // возвращаем opaque-заглушку (0)
            return llvm::ConstantInt::get(llvm::IntegerType::get(*ctx_, 256), 0);
        }
        case ExprKind::Call: {
            const auto& c = static_cast<const CallExpr&>(e);
            auto* i256 = llvm::IntegerType::get(*ctx_, 256);
            // внутренний вызов функции контракта — реальный call (для межпроцедурного
            // анализа reentrancy через внутренние вызовы)
            if (c.callee->kind == ExprKind::Ident) {
                const auto& id = static_cast<const IdentExpr&>(*c.callee);
                auto it = funcByName_.find(id.name);
                if (it != funcByName_.end()) {
                    llvm::Function* callee = it->second;
                    unsigned n = callee->arg_size();
                    std::vector<llvm::Value*> args(n, nullptr);
                    unsigned k = 0;
                    for (const auto& a : c.args) {
                        auto* v = emitExpr(*a);
                        if (!v) return nullptr;
                        if (k < n) {
                            auto* pty = callee->getArg(k)->getType();
                            if (v->getType() != pty && v->getType()->isIntegerTy() && pty->isIntegerTy()) {
                                unsigned vb = v->getType()->getIntegerBitWidth();
                                unsigned pb = pty->getIntegerBitWidth();
                                v = vb < pb ? builder_->CreateZExt(v, pty)
                                            : (vb > pb ? builder_->CreateTrunc(v, pty) : v);
                            }
                            args[k] = v;
                        }
                        ++k;
                    }
                    for (unsigned j = 0; j < n; ++j)
                        if (!args[j]) args[j] = llvm::Constant::getNullValue(callee->getArg(j)->getType());
                    auto* call = builder_->CreateCall(callee, args);
                    return callee->getReturnType()->isVoidTy()
                               ? static_cast<llvm::Value*>(llvm::ConstantInt::get(i256, 0))
                               : static_cast<llvm::Value*>(call);
                }
            }
            // высокоуровневый ВНЕШНИЙ метод-вызов obj.method(args) / каст Type(x) — opaque
            for (const auto& a : c.args) emitExpr(*a);
            return llvm::ConstantInt::get(i256, 0);
        }
        case ExprKind::Cond: {
            const auto& ce = static_cast<const CondExpr&>(e);
            auto* c = emitExpr(*ce.cond);
            auto* t = emitExpr(*ce.thenE);
            auto* f = emitExpr(*ce.elseE);
            if (!c || !t || !f) return nullptr;
            // select требует одинаковую ширину ветвей — приводим уже целочисленно
            if (t->getType() != f->getType() &&
                t->getType()->isIntegerTy() && f->getType()->isIntegerTy()) {
                unsigned tb = t->getType()->getIntegerBitWidth();
                unsigned fb = f->getType()->getIntegerBitWidth();
                if (tb < fb) t = builder_->CreateZExt(t, f->getType(), "condext");
                else         f = builder_->CreateZExt(f, t->getType(), "condext");
            }
            if (t->getType() != f->getType()) return t;  // несводимо — берём then (opaque)
            return builder_->CreateSelect(c, t, f, "cond");
        }
        case ExprKind::Index: {
            const auto& ix = static_cast<const IndexExpr&>(e);
            auto* i256 = llvm::IntegerType::get(*ctx_, 256);
            // индексация массива/opaque (база не mapping) — заглушка 0
            if (ix.base->resolvedType->kind != ast::TypeKind::Mapping)
                return llvm::ConstantInt::get(i256, 0);
            auto* ptr = emitMapElemPtr(ix);
            if (!ptr) return nullptr;
            auto* load = builder_->CreateLoad(i256, ptr, "mload");
            // атрибутируем к базовому слоту mapping
            const auto& baseId = static_cast<const IdentExpr&>(*ix.base);
            tagStorage(load, baseId.resolvedSlot);
            auto* targetTy = llvmType(ix.base->resolvedType->mapValue);
            if (targetTy->isIntegerTy(256)) return load;
            return builder_->CreateTrunc(load, targetTy, "trunc");
        }
        case ExprKind::LowLevelCall: {
            const auto& lc = static_cast<const LowLevelCallExpr&>(e);
            auto* target = emitExpr(*lc.target);  // i160 адрес получателя
            auto* value  = emitExpr(*lc.value);   // i256 количество wei (0 для delegatecall)
            if (!target || !value) return nullptr;
            // @scsa.external_call — маркер: управление уходит наружу, возможен callback
            auto* call = builder_->CreateCall(
                intr_.external_call, {target, value}, "extcall");
            tagExternalCall(call, lc.kind == LowLevelKind::DelegateCall ? "delegatecall" : "call");
            // low-level .call/.delegatecall: bool success можно отбросить (SWC-104) -> метка raw
            call->setMetadata("scsa.call.raw", llvm::MDNode::get(*ctx_, {}));
            return call;  // i1 флаг успеха
        }
        default:
            diag_.error(e.loc, "expr codegen not implemented yet");
            return nullptr;
    }
}

void CodeGen::emitStmt(const ast::Stmt& s) {
    using namespace ast;
    // защита: если текущий блок уже терминирован (например, после return внутри if),
    // последующие statements не должны генерировать инструкции — это незаконно в LLVM IR
    if (builder_->GetInsertBlock()->getTerminator()) return;
    // все инструкции этого statement (и вложенных выражений) получат !dbg с этой позицией
    setDebugLoc(s.loc);
    switch (s.kind) {
        case StmtKind::VarDecl: {
            const auto& v = static_cast<const VarDeclStmt&>(s);
            // var acc = mapping[key] — storage-ССЫЛКА (старый Solidity, только для 'var'
            // над storage-агрегатом): acc.field = ... пишет в саму ячейку. записываем
            // алиас (типизированный `uint256 a = map[k]` — это КОПИЯ-значение, не ссылка,
            // поэтому гейт по v.inferred). локаль всё равно создаём — для bare-обращений
            if (v.inferred && v.init->kind == ExprKind::Index) {
                const auto& ix = static_cast<const IndexExpr&>(*v.init);
                if (ix.base->kind == ExprKind::Ident && ix.base->resolvedType &&
                    ix.base->resolvedType->kind == ast::TypeKind::Mapping) {
                    const auto& baseId = static_cast<const IdentExpr&>(*ix.base);
                    storageAliases_[v.name] = StorageAlias{baseId.resolvedSlot, ix.index.get()};
                }
            }
            auto* val = emitExpr(*v.init);
            if (!val) return;
            auto* ty  = llvmType(v.type.kind);
            // коэрция init к типу локали (var-вывод/opaque могут дать иную ширину)
            if (val->getType() != ty && val->getType()->isIntegerTy() && ty->isIntegerTy()) {
                unsigned vb = val->getType()->getIntegerBitWidth();
                unsigned tb = ty->getIntegerBitWidth();
                val = vb < tb ? builder_->CreateZExt(val, ty, "vext")
                              : builder_->CreateTrunc(val, ty, "vtrunc");
            }
            auto* alc = builder_->CreateAlloca(ty, nullptr, v.name);
            builder_->CreateStore(val, alc);
            locals_[v.name] = LocalSlot{alc, ty};
            return;
        }
        case StmtKind::Assign: {
            const auto& a = static_cast<const AssignStmt&>(s);
            auto* val = emitExpr(*a.value);
            if (!val) return;

            // acc.field = val, где acc — storage-ссылка (var acc = map[key]): это
            // запись в саму ячейку storage. критично для reentrancy: без этого запись
            // после external call теряется (honeypot-паттерн PrivateBank)
            if (a.target->kind == ExprKind::Member) {
                const auto& m = static_cast<const MemberExpr&>(*a.target);
                if (m.base->kind == ExprKind::Ident) {
                    auto ai = storageAliases_.find(
                        static_cast<const IdentExpr&>(*m.base).name);
                    if (ai != storageAliases_.end()) {
                        auto* i256 = llvm::IntegerType::get(*ctx_, 256);
                        auto* ptr = emitMapElemPtrRaw(ai->second.baseSlot, *ai->second.key);
                        if (!ptr) return;
                        llvm::Value* wide = val;
                        if (!wide->getType()->isIntegerTy(256))
                            wide = builder_->CreateZExt(wide, i256, "zext");
                        if (a.compound) {
                            auto* cur = builder_->CreateLoad(i256, ptr, "mload");
                            tagStorage(cur, ai->second.baseSlot);
                            wide = (*a.compound == BinOp::Add)
                                       ? builder_->CreateAdd(cur, wide, "addeq")
                                       : builder_->CreateSub(cur, wide, "subeq");
                        }
                        auto* st = builder_->CreateStore(wide, ptr);
                        tagStorage(st, ai->second.baseSlot);
                        return;
                    }
                }
            }

            // присваивание в поле struct (Member) или элемент массива/opaque (Index
            // с базой-не-mapping) — семантику агрегатов не моделируем, no-op (значение
            // уже эмитировано ради побочных эффектов). НО если запись укоренена в storage
            // (state-var агрегат) — ставим маркер @scsa.opaque_store, чтобы детектор честно
            // предупредил о возможной reentrancy, а не молча пропустил (ложный негатив)
            if (a.target->kind == ExprKind::Member ||
                (a.target->kind == ExprKind::Index &&
                 static_cast<const IndexExpr&>(*a.target).base->resolvedType->kind
                     != ast::TypeKind::Mapping)) {
                if (rootsAtStorage(*a.target))
                    builder_->CreateCall(intr_.opaque_store, {});
                return;
            }

            // составное присваивание += / -= : читаем текущее значение lvalue и
            // комбинируем (read-modify-write). emitExpr(target) даёт текущее значение
            if (a.compound) {
                auto* cur = emitExpr(*a.target);
                if (!cur) return;
                val = (*a.compound == BinOp::Add)
                          ? builder_->CreateAdd(cur, val, "addeq")
                          : builder_->CreateSub(cur, val, "subeq");
            }

            // lvalue-элемент mapping: balances[k] = val — запись через map_elem
            if (a.target->kind == ExprKind::Index) {
                const auto& ix = static_cast<const IndexExpr&>(*a.target);
                auto* ptr = emitMapElemPtr(ix);
                if (!ptr) return;
                auto* i256 = llvm::IntegerType::get(*ctx_, 256);
                llvm::Value* wide = val;
                if (!val->getType()->isIntegerTy(256)) wide = builder_->CreateZExt(val, i256, "zext");
                auto* st = builder_->CreateStore(wide, ptr);
                const auto& baseId = static_cast<const IdentExpr&>(*ix.base);
                tagStorage(st, baseId.resolvedSlot);
                return;
            }

            // lvalue — простое имя: local/param или state var
            const auto& id = static_cast<const IdentExpr&>(*a.target);
            auto it = locals_.find(id.name);
            if (it != locals_.end()) {
                builder_->CreateStore(val, it->second.alloca);
                return;
            }
            auto sit = stateVarSlots_.find(id.name);
            if (sit == stateVarSlots_.end()) {
                diag_.error(a.loc, "internal: unknown assign target");
                return;
            }
            emitStateStore(sit->second, val, stateVarTypes_[id.name]);
            return;
        }
        case StmtKind::If: {
            const auto& ifs = static_cast<const IfStmt&>(s);
            auto* cond = emitExpr(*ifs.cond);
            if (!cond) return;

            auto* thenBB  = llvm::BasicBlock::Create(*ctx_, "if.then",  currentFn_);
            auto* elseBB  = llvm::BasicBlock::Create(*ctx_, "if.else",  currentFn_);
            auto* mergeBB = llvm::BasicBlock::Create(*ctx_, "if.merge", currentFn_);

            builder_->CreateCondBr(cond, thenBB, elseBB);

            // then-ветка
            builder_->SetInsertPoint(thenBB);
            for (const auto& sp : ifs.thenBlock) emitStmt(*sp);
            if (!builder_->GetInsertBlock()->getTerminator()) {
                builder_->CreateBr(mergeBB);
            }

            // else-ветка
            builder_->SetInsertPoint(elseBB);
            for (const auto& sp : ifs.elseBlock) emitStmt(*sp);
            if (!builder_->GetInsertBlock()->getTerminator()) {
                builder_->CreateBr(mergeBB);
            }

            // если в merge никто не приходит — обе ветки терминированы (например, return/return),
            // тогда merge — мёртвый блок, удаляем; builder остаётся в elseBB, guard в начале
            // emitStmt отшлёпает любые дальнейшие statements
            if (mergeBB->hasNPredecessors(0)) {
                mergeBB->eraseFromParent();
            } else {
                builder_->SetInsertPoint(mergeBB);
            }
            return;
        }
        case StmtKind::Return: {
            const auto& r = static_cast<const ReturnStmt&>(s);
            if (r.value) {
                auto* v = emitExpr(*r.value);
                if (!v) return;
                // коэрция к типу возврата функции: opaque-значение (каст/keccak → i256)
                // может не совпадать по ширине с объявленным типом (напр. bytes32→i160).
                // приводим целочисленно zext/trunc, иначе verifyModule упадёт
                auto* rty = currentFn_->getReturnType();
                if (v->getType() != rty && v->getType()->isIntegerTy() && rty->isIntegerTy()) {
                    unsigned vb = v->getType()->getIntegerBitWidth();
                    unsigned rb = rty->getIntegerBitWidth();
                    v = vb < rb ? builder_->CreateZExt(v, rty, "retext")
                                : builder_->CreateTrunc(v, rty, "rettrunc");
                }
                builder_->CreateRet(v);
            } else {
                builder_->CreateRetVoid();
            }
            return;
        }
        case StmtKind::Require: {
            const auto& r = static_cast<const RequireStmt&>(s);
            auto* cond = emitExpr(*r.cond);
            if (!cond) return;

            // require(cond) → br i1 cond, ok, fail; fail: call @evm.revert + unreachable
            auto* okBB   = llvm::BasicBlock::Create(*ctx_, "require.ok",   currentFn_);
            auto* failBB = llvm::BasicBlock::Create(*ctx_, "require.fail", currentFn_);

            builder_->CreateCondBr(cond, okBB, failBB);

            builder_->SetInsertPoint(failBB);
            builder_->CreateCall(intr_.revert, {});
            // evm.revert помечен noreturn, поэтому после него — unreachable.
            // Это и terminator (LLVM требует), и подсказка анализаторам, что путь мёртвый.
            builder_->CreateUnreachable();

            builder_->SetInsertPoint(okBB);
            return;
        }
        case StmtKind::Expr: {
            const auto& es = static_cast<const ExprStmt&>(s);
            emitExpr(*es.expr);   // внешний вызов; результат не используется
            return;
        }
        case StmtKind::Throw: {
            // throw == безусловный revert (как ветка require.fail)
            builder_->CreateCall(intr_.revert, {});
            builder_->CreateUnreachable();
            return;
        }
        case StmtKind::Placeholder:
            // _; эмитится через emitWithModifiers; напрямую сюда не попадает — no-op
            return;
        case StmtKind::Selfdestruct: {
            const auto& sd = static_cast<const SelfdestructStmt&>(s);
            auto* r = emitExpr(*sd.recipient);   // i160 адрес
            if (!r) return;
            builder_->CreateCall(intr_.self_destruct, {r});
            return;
        }
        default:
            // остальные кейсы — следующие задачи
            return;
    }
}

// инициализация debug-info: флаги модуля, DIBuilder, файл и compile unit.
// имя файла — имя контракта + ".sol"; язык C, producer "scsa"
void CodeGen::initDebugInfo(const ast::ContractDecl& c) {
    // версия формата отладочной информации и версия DWARF — обязательные флаги,
    // иначе verifyModule ругается на несогласованные метаданные
    module_->addModuleFlag(llvm::Module::Warning, "Debug Info Version",
                           llvm::DEBUG_METADATA_VERSION);
    module_->addModuleFlag(llvm::Module::Warning, "Dwarf Version", 4);

    dib_    = std::make_unique<llvm::DIBuilder>(*module_);
    diFile_ = dib_->createFile(c.name + ".sol", ".");
    // isOptimized=false, флаги пустые, RuntimeVersion=0 — минимальный CU
    diCU_   = dib_->createCompileUnit(
        llvm::dwarf::DW_LANG_C, diFile_, "scsa",
        /*isOptimized=*/false, /*flags=*/"", /*RuntimeVersion=*/0);
}

// выставить текущую отладочную позицию для последующих инструкций.
// область — текущая DISubprogram (diSP_); без неё !dbg ставить нельзя
void CodeGen::setDebugLoc(const SourceLoc& loc) {
    if (!diSP_) return;
    builder_->SetCurrentDebugLocation(
        llvm::DILocation::get(*ctx_, loc.line, loc.col, diSP_));
}

std::unique_ptr<llvm::Module> CodeGen::generate(const ast::ContractDecl& c) {
    ctx_     = std::make_unique<llvm::LLVMContext>();
    module_  = std::make_unique<llvm::Module>(c.name, *ctx_);
    builder_ = std::make_unique<llvm::IRBuilder<>>(*ctx_);
    // указатели EVM-адрес-пространств (storage/memory/calldata) — 256-битные,
    // как ключи storage в EVM; тогда inttoptr i256 в них идёт без усечения
    module_->setDataLayout("e-p1:256:256-p2:256:256-p3:256:256");
    intr_    = declareEvmIntrinsics(*module_);

    // debug-info должен быть готов до эмиссии функций: каждой функции нужна DISubprogram
    initDebugInfo(c);

    stateVarSlots_.clear();
    stateVarTypes_.clear();
    for (const auto& sv : c.stateVars) {
        stateVarSlots_[sv.name] = sv.storageSlot;
        stateVarTypes_[sv.name] = sv.type.kind;
    }

    // карта модификаторов по имени — для инлайнинга в тела функций
    modifierMap_.clear();
    for (const auto& md : c.modifiers) modifierMap_[md.name] = &md;

    // два прохода: сначала все прототипы (чтобы внутренние вызовы видели любую
    // функцию, в т.ч. объявленную ниже), затем тела
    funcByName_.clear();
    for (const auto& fn : c.functions)
        funcByName_[fn.name] = emitFunctionProto(fn);
    for (const auto& fn : c.functions)
        emitFunctionBody(fn, funcByName_[fn.name]);

    // finalize после эмиссии всех функций и до verifyModule: DIBuilder
    // достраивает отложенные метаданные (списки, ссылки CU->subprograms)
    dib_->finalize();

    if (llvm::verifyModule(*module_, nullptr)) {
        diag_.error({}, "internal: module failed verification");
        return nullptr;
    }
    return std::move(module_);
}

}  // namespace solc::codegen
