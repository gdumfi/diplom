#include "solc/codegen/AstJsonLowering.h"

#include <algorithm>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Verifier.h>

#include "solc/driver/SolcRunner.h"   // buildIdIndex

namespace solc::codegen {

using JObj = llvm::json::Object;
using JVal = llvm::json::Value;

// named DEX-роутер V2/V3 (для вызова на КОНТРАКТЕ). БЕЗ generic `swap`: контракт-метод `.swap()`
// часто = защищённый свопер (BeefySwapper со slippage/оракулом внутри) → иначе массовый FP
static bool isRouterSwap(llvm::StringRef m) {
    return m == "swapExactTokensForTokens" || m == "swapTokensForExactTokens" ||
           m == "swapExactETHForTokens" || m == "swapExactTokensForETH" ||
           m == "swapExactTokensForTokensSupportingFeeOnTransferTokens" ||
           m == "exactInputSingle" || m == "exactInput" ||
           m == "exactOutputSingle" || m == "exactOutput";
}
// + generic `swap` — ТОЛЬКО для library-helper (DexSwap.swap: raw pair-swap без slippage, иначе
// opaque в IR). библиотека-helper делает pair-математику сама → реальный slippage-риск
static bool isSwapMethod(llvm::StringRef m) { return isRouterSwap(m) || m == "swap"; }

AstJsonLowering::AstJsonLowering(DiagnosticEngine& diag) : diag_(diag) {}

// -------- мелкие аксессоры solc-AST --------

llvm::StringRef AstJsonLowering::nodeType(const JObj& n) {
    auto s = n.getString("nodeType");
    return s ? *s : llvm::StringRef{};
}
llvm::StringRef AstJsonLowering::strField(const JObj& n, llvm::StringRef k) {
    auto s = n.getString(k);
    return s ? *s : llvm::StringRef{};
}
const JObj* AstJsonLowering::nodeById(int64_t id) const {
    auto it = idIndex_.find(id);
    return it == idIndex_.end() ? nullptr : it->second;
}
int AstJsonLowering::slotOfDecl(int64_t declId) const {
    auto it = slotById_.find(declId);
    return it == slotById_.end() ? -1 : it->second;
}
// typeString узла (для выбора ширины/трункейта)
static llvm::StringRef typeStrOf(const JObj& n) {
    if (const auto* td = n.getObject("typeDescriptions"))
        if (auto s = td->getString("typeString")) return *s;
    return {};
}

// канонический ABI-тип из solc typeString — для ВЕРНОГО 4-байтного селектора в PoC.
// address payable/contract/enum → address/address/uint8; сохраняем ширину uint128,
// bytes32, массивы. struct/tuple не кодируем (fallback uint256). нужно потому, что в
// нашем IR все uint схлопнуты в i256 и ширина по llvm-типу невосстановима
static std::string abiTypeOf(llvm::StringRef tsr) {
    std::string ts = tsr.str();
    for (const char* suf : {" storage pointer", " storage ref", " storage",
                            " memory", " calldata", " payable"}) {
        auto p = ts.find(suf);
        if (p != std::string::npos) ts.erase(p, std::string(suf).size());
    }
    auto starts = [&](const char* p) { return ts.rfind(p, 0) == 0; };
    auto arraySuffix = [&]() -> std::string {
        auto b = ts.find('[');
        return b == std::string::npos ? std::string{} : ts.substr(b);
    };
    if (starts("address"))   return "address" + arraySuffix();
    if (starts("contract ")) return "address" + arraySuffix();
    if (starts("enum "))     return "uint8" + arraySuffix();
    if (starts("uint") || starts("int") || starts("bytes") ||
        starts("bool") || starts("string"))
        return ts;   // uint128 / bytes32 / uint256[] / bool / string — годятся как есть
    return "uint256";   // struct/tuple/прочее — консервативный fallback
}

// -------- типы --------

llvm::Type* AstJsonLowering::typeFromStr(llvm::StringRef ts) {
    auto* i1   = llvm::Type::getInt1Ty(*ctx_);
    auto* i160 = llvm::IntegerType::get(*ctx_, 160);
    auto* i256 = llvm::IntegerType::get(*ctx_, 256);
    if (ts.starts_with("bool")) return i1;
    if (ts.starts_with("address")) return i160;
    if (ts.starts_with("contract ")) return i160;   // адресоподобный
    if (ts.starts_with("uint") || ts.starts_with("int")) return i256;
    return i256;   // bytes/string/struct/enum/… — opaque i256
}

// -------- storage / интринсики (форма IR-контракта, идентична CodeGen) --------

llvm::Value* AstJsonLowering::storageSlotPtr(int slot) {
    auto* i256 = llvm::IntegerType::get(*ctx_, 256);
    auto* ptrTy = llvm::PointerType::get(*ctx_, kStorageAS);
    return builder_->CreateIntToPtr(llvm::ConstantInt::get(i256, slot), ptrTy, "slot.ptr");
}
llvm::Value* AstJsonLowering::emitMapElemPtrRaw(int baseSlot, const JObj& keyExpr) {
    auto* i256 = llvm::IntegerType::get(*ctx_, 256);
    auto* baseSlotC = llvm::ConstantInt::get(i256, baseSlot);
    auto* key = emitExpr(keyExpr);
    if (!key) return nullptr;
    if (!key->getType()->isIntegerTy(256)) key = builder_->CreateZExt(key, i256, "keyext");
    return builder_->CreateCall(intr_.map_elem, {baseSlotC, key}, "map.elem");
}
void AstJsonLowering::tagStorage(llvm::Instruction* inst, int slot) {
    auto* i64 = llvm::IntegerType::get(*ctx_, 64);
    auto* md  = llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64, slot));
    inst->setMetadata("scsa.storage", llvm::MDNode::get(*ctx_, {md}));
}
void AstJsonLowering::tagExternalCall(llvm::Instruction* call, const char* kindStr) {
    call->setMetadata("scsa.call.kind",
                      llvm::MDNode::get(*ctx_, {llvm::MDString::get(*ctx_, kindStr)}));
    auto* i1 = llvm::IntegerType::get(*ctx_, 1);
    auto* untrusted = llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i1, 0));
    call->setMetadata("scsa.call.trusted", llvm::MDNode::get(*ctx_, {untrusted}));
}
llvm::Value* AstJsonLowering::emitStateLoad(int slot, llvm::StringRef typeStr) {
    auto* i256 = llvm::IntegerType::get(*ctx_, 256);
    auto* load = builder_->CreateLoad(i256, storageSlotPtr(slot), "sload");
    tagStorage(load, slot);
    auto* target = typeFromStr(typeStr);
    if (target->isIntegerTy(256)) return load;
    return builder_->CreateTrunc(load, target, "trunc");   // bool/address узкие
}
void AstJsonLowering::emitStateStore(int slot, llvm::Value* v) {
    auto* i256 = llvm::IntegerType::get(*ctx_, 256);
    llvm::Value* wide = v->getType()->isIntegerTy(256) ? v
                        : builder_->CreateZExt(v, i256, "zext");
    auto* st = builder_->CreateStore(wide, storageSlotPtr(slot));
    tagStorage(st, slot);
}

// синтетический слот для поля mapping-of-struct: (базовый слот mapping, offset поля) ->
// уникальный слот НАД реальными (ленивая непрерывная нумерация; массив KLEE не раздувается).
// одинаковая пара всегда даёт тот же слот -> доступ до и после callback алиасится
int AstJsonLowering::synthSlotFor(int baseSlot, int memberOffset) {
    auto key = std::make_pair(baseSlot, memberOffset);
    auto it = synthSlot_.find(key);
    if (it != synthSlot_.end()) return it->second;
    int s = synthNext_++;
    synthSlot_[key] = s;
    return s;
}

// ячейка storage для доступа к полю struct X.field. возвращает указатель addrspace(1)
// и слот для метки. поддержаны: mapping(=>struct)[k].field, plain-struct-var.field,
// storage-ссылка acc.field. иначе nullopt (поле не моделируем -> opaque_store)
std::optional<AstJsonLowering::FieldCell>
AstJsonLowering::structFieldCell(const JObj& ma) {
    auto ref = ma.getInteger("referencedDeclaration");
    if (!ref) return std::nullopt;
    auto mit = memberSlotById_.find(*ref);
    if (mit == memberSlotById_.end()) return std::nullopt;   // не поле struct
    int memberOffset = mit->second;
    const auto* base = ma.getObject("expression");
    if (!base) return std::nullopt;

    // mapping(=>struct)[key].field
    if (nodeType(*base) == "IndexAccess") {
        const auto* be = base->getObject("baseExpression");
        const auto* ie = base->getObject("indexExpression");
        if (be && ie && typeStrOf(*be).starts_with("mapping") && nodeType(*be) == "Identifier") {
            int baseSlot = -1;
            if (auto r = be->getInteger("referencedDeclaration")) baseSlot = slotOfDecl(*r);
            if (baseSlot >= 0) {
                int slot = synthSlotFor(baseSlot, memberOffset);
                auto* ptr = emitMapElemPtrRaw(slot, *ie);
                if (!ptr) return std::nullopt;
                return FieldCell{ptr, slot};
            }
        }
        return std::nullopt;
    }
    if (nodeType(*base) == "Identifier") {
        auto bname = strField(*base, "name");
        // storage-ссылка acc.field (T storage acc = map[k])
        auto ai = storageAliases_.find(bname.str());
        if (ai != storageAliases_.end()) {
            int slot = synthSlotFor(ai->second.baseSlot, memberOffset);
            auto* ptr = emitMapElemPtrRaw(slot, *ai->second.key);
            if (!ptr) return std::nullopt;
            return FieldCell{ptr, slot};
        }
        // plain-struct-var.field -> реальный последовательный слот (истинный EVM-layout)
        if (auto r = base->getInteger("referencedDeclaration")) {
            int structSlot = slotOfDecl(*r);
            if (structSlot >= 0) {
                int slot = structSlot + memberOffset;
                return FieldCell{storageSlotPtr(slot), slot};
            }
        }
    }
    return std::nullopt;
}

bool AstJsonLowering::rootsAtStorage(const JObj& e) const {
    auto nt = nodeType(e);
    if (nt == "Identifier") {
        auto id = e.getInteger("referencedDeclaration");
        return id && slotById_.count(*id) > 0;
    }
    if (nt == "MemberAccess")
        if (const auto* b = e.getObject("expression")) return rootsAtStorage(*b);
    if (nt == "IndexAccess")
        if (const auto* b = e.getObject("baseExpression")) return rootsAtStorage(*b);
    return false;
}

// приведение двух целочисленных значений к общей ширине (zext более узкого)
static void matchWidths(llvm::IRBuilder<>& b, llvm::Value*& L, llvm::Value*& R) {
    if (!L->getType()->isIntegerTy() || !R->getType()->isIntegerTy()) return;
    unsigned lb = L->getType()->getIntegerBitWidth(), rb = R->getType()->getIntegerBitWidth();
    if (lb == rb) return;
    if (lb < rb) L = b.CreateZExt(L, R->getType(), "wext");
    else         R = b.CreateZExt(R, L->getType(), "wext");
}

// -------- debug-info --------

// разобрать src solc "start:len:fileIndex" в смещение и индекс файла
static void parseSrc(llvm::StringRef src, long& off, int& fileIdx) {
    off = 0; fileIdx = 0;
    size_t c1 = src.find(':');
    src.substr(0, c1).getAsInteger(10, off);
    if (c1 == llvm::StringRef::npos) return;
    size_t c2 = src.find(':', c1 + 1);
    if (c2 == llvm::StringRef::npos) return;
    long f = 0; src.substr(c2 + 1).getAsInteger(10, f); fileIdx = static_cast<int>(f);
}
std::vector<size_t> AstJsonLowering::buildLineStarts(const std::string& source) {
    std::vector<size_t> v;
    v.push_back(0);
    for (size_t i = 0; i < source.size(); ++i)
        if (source[i] == '\n') v.push_back(i + 1);
    return v;
}
unsigned AstJsonLowering::lineOfSrc(llvm::StringRef src) const {
    long off; int fileIdx;
    parseSrc(src, off, fileIdx);
    auto it = fileLineTables_.find(fileIdx);
    if (it == fileLineTables_.end()) return 1;
    auto u = std::upper_bound(it->second.begin(), it->second.end(), static_cast<size_t>(off));
    return static_cast<unsigned>(u - it->second.begin());   // 1-based
}
void AstJsonLowering::setDebugLocFromSrc(const JObj& n) {
    if (!diSP_) return;
    auto src = n.getString("src");
    if (!src) return;
    long off; int fileIdx;
    parseSrc(*src, off, fileIdx);
    unsigned line = lineOfSrc(*src), col = 1;
    auto it = fileLineTables_.find(fileIdx);
    if (it != fileLineTables_.end() && line >= 1 && line <= it->second.size())
        col = static_cast<unsigned>(off - it->second[line - 1] + 1);
    builder_->SetCurrentDebugLocation(llvm::DILocation::get(*ctx_, line, col, diSP_));
}
void AstJsonLowering::initDebugInfo(const std::string& contractName) {
    module_->addModuleFlag(llvm::Module::Warning, "Debug Info Version",
                           llvm::DEBUG_METADATA_VERSION);
    module_->addModuleFlag(llvm::Module::Warning, "Dwarf Version", 4);
    dib_    = std::make_unique<llvm::DIBuilder>(*module_);
    diFile_ = dib_->createFile(unit_, ".");
    diCU_   = dib_->createCompileUnit(llvm::dwarf::DW_LANG_C, diFile_, "scsa",
                                      /*isOptimized=*/false, /*flags=*/"", /*RuntimeVersion=*/0);
    (void)contractName;
}

// -------- выражения --------

llvm::Value* AstJsonLowering::emitExpr(const JObj& e) {
    struct Guard { int& d; Guard(int& x): d(x){ ++d; } ~Guard(){ --d; } } g(exprDepth_);
    if (exprDepth_ > kMaxExprDepth) return nullptr;
    auto* i256 = llvm::IntegerType::get(*ctx_, 256);
    auto nt = nodeType(e);

    if (nt == "Literal") {
        auto kind = strField(e, "kind");
        if (kind == "bool")
            return llvm::ConstantInt::get(llvm::Type::getInt1Ty(*ctx_),
                                          strField(e, "value") == "true" ? 1 : 0);
        if (kind == "number") {
            llvm::StringRef val = strField(e, "value");
            bool hex = val.starts_with("0x") || val.starts_with("0X");
            llvm::StringRef digits = hex ? val.substr(2) : val;
            unsigned radix = hex ? 16 : 10;
            if (digits.empty() || llvm::APInt::getBitsNeeded(digits, radix) > 256)
                return llvm::ConstantInt::get(i256, 0);
            llvm::APInt v(256, digits, radix);
            // субденоминации: 1 ether / 5 days и т.п.
            auto sub = strField(e, "subdenomination");
            uint64_t mul = 0;
            if      (sub == "wei" || sub == "seconds") mul = 1;
            else if (sub == "gwei")    mul = 1000000000ULL;
            else if (sub == "ether")   { v *= llvm::APInt(256, 1000000000000000000ULL); mul = 0; }
            else if (sub == "minutes") mul = 60;
            else if (sub == "hours")   mul = 3600;
            else if (sub == "days")    mul = 86400;
            else if (sub == "weeks")   mul = 604800;
            if (mul > 1) v *= llvm::APInt(256, mul);
            return llvm::ConstantInt::get(*ctx_, v);
        }
        return llvm::ConstantInt::get(i256, 0);   // строки/hexString — opaque
    }

    if (nt == "Identifier") {
        auto name = strField(e, "name");
        auto refOpt = e.getInteger("referencedDeclaration");
        int64_t ref = refOpt ? *refOpt : 0;
        // стейт-переменная (по astId из storageLayout)
        if (ref > 0) {
            auto it = slotById_.find(ref);
            if (it != slotById_.end())
                return emitStateLoad(it->second, typeStrById_.count(ref) ? typeStrById_[ref] : typeStrOf(e));
        }
        // локаль/параметр
        auto lit = locals_.find(name.str());
        if (lit != locals_.end())
            return builder_->CreateLoad(lit->second.type, lit->second.alloca, name);
        // билтины/прочее
        if (name == "now") return builder_->CreateCall(intr_.block_timestamp, {}, "now");
        if (name == "this") return llvm::ConstantInt::get(llvm::IntegerType::get(*ctx_, 160), 0);
        return llvm::ConstantInt::get(i256, 0);   // opaque
    }

    if (nt == "BinaryOperation") {
        auto* L = e.getObject("leftExpression")  ? emitExpr(*e.getObject("leftExpression"))  : nullptr;
        auto* R = e.getObject("rightExpression") ? emitExpr(*e.getObject("rightExpression")) : nullptr;
        if (!L || !R) return nullptr;
        auto op = strField(e, "operator");
        if (op == "&&") { matchWidths(*builder_, L, R); return builder_->CreateAnd(L, R, "and"); }
        if (op == "||") { matchWidths(*builder_, L, R); return builder_->CreateOr(L, R, "or"); }
        matchWidths(*builder_, L, R);
        if (op == "+")  return builder_->CreateAdd(L, R, "add");
        if (op == "-")  return builder_->CreateSub(L, R, "sub");
        if (op == "*")  return builder_->CreateMul(L, R, "mul");
        if (op == "/")  return builder_->CreateUDiv(L, R, "udiv");
        if (op == "%")  return builder_->CreateURem(L, R, "urem");
        if (op == "&")  return builder_->CreateAnd(L, R, "band");
        if (op == "|")  return builder_->CreateOr(L, R, "bor");
        if (op == "^")  return builder_->CreateXor(L, R, "bxor");
        if (op == "<<") return builder_->CreateShl(L, R, "shl");
        if (op == ">>") return builder_->CreateLShr(L, R, "lshr");
        if (op == "==") return builder_->CreateICmpEQ(L, R, "eq");
        if (op == "!=") return builder_->CreateICmpNE(L, R, "ne");
        if (op == "<")  return builder_->CreateICmpULT(L, R, "ult");
        if (op == ">")  return builder_->CreateICmpUGT(L, R, "ugt");
        if (op == "<=") return builder_->CreateICmpULE(L, R, "ule");
        if (op == ">=") return builder_->CreateICmpUGE(L, R, "uge");
        return L;
    }

    if (nt == "UnaryOperation") {
        auto* V = e.getObject("subExpression") ? emitExpr(*e.getObject("subExpression")) : nullptr;
        if (!V) return nullptr;
        auto op = strField(e, "operator");
        if (op == "!") return builder_->CreateXor(V, llvm::ConstantInt::get(V->getType(), 1), "not");
        if (op == "~") return builder_->CreateXor(V, llvm::ConstantInt::getAllOnesValue(V->getType()), "bnot");
        if (op == "-") return builder_->CreateNeg(V, "neg");
        if (op == "++" || op == "--") {
            // read-modify-write на lvalue: V текущее, пишем V±1
            auto* one = llvm::ConstantInt::get(V->getType(), 1);
            auto* nv = op == "++" ? builder_->CreateAdd(V, one, "inc")
                                  : builder_->CreateSub(V, one, "dec");
            if (const auto* sub = e.getObject("subExpression")) storeToLValue(*sub, nv);
            return strField(e, "prefix") == "true" ? nv : V;
        }
        return V;
    }

    if (nt == "Assignment")   return emitAssignment(e);
    if (nt == "FunctionCall") return emitFunctionCall(e);

    if (nt == "MemberAccess") {
        auto member = strField(e, "memberName");
        const auto* base = e.getObject("expression");
        if (base && nodeType(*base) == "Identifier") {
            auto bname = strField(*base, "name");
            if (bname == "msg" && member == "sender")
                return builder_->CreateCall(intr_.msg_sender, {}, "msg.sender");
            if (bname == "msg" && member == "value")
                return builder_->CreateCall(intr_.msg_value, {}, "msg.value");
            if (bname == "tx" && member == "origin")
                return builder_->CreateCall(intr_.tx_origin, {}, "tx.origin");
            if (bname == "block" && member == "timestamp")
                return builder_->CreateCall(intr_.block_timestamp, {}, "block.timestamp");
            if (bname == "block" && member == "number")
                return builder_->CreateCall(intr_.block_number, {}, "block.number");
            // acc.field, где acc — storage-ссылка: чтение ячейки storage
            auto ai = storageAliases_.find(bname.str());
            if (ai != storageAliases_.end() && !e.getInteger("referencedDeclaration")) {
                // bare acc (без поля) как storage-ссылка — единичная ячейка mapping
                auto* ptr = emitMapElemPtrRaw(ai->second.baseSlot, *ai->second.key);
                if (!ptr) return nullptr;
                auto* load = builder_->CreateLoad(i256, ptr, "mload");
                tagStorage(load, ai->second.baseSlot);
                return load;
            }
        }
        // поле struct (real/synthetic слот) — чтение ячейки storage
        if (auto cell = structFieldCell(e)) {
            auto* load = builder_->CreateLoad(i256, cell->ptr, "fload");
            tagStorage(load, cell->slot);
            auto* target = typeFromStr(typeStrOf(e));
            return target->isIntegerTy(256) ? load : builder_->CreateTrunc(load, target, "trunc");
        }
        return llvm::ConstantInt::get(i256, 0);   // .length/.balance/прочее — opaque
    }

    if (nt == "IndexAccess") {
        const auto* baseE = e.getObject("baseExpression");
        const auto* idxE  = e.getObject("indexExpression");
        // mapping? определяем по typeString базы
        bool isMapping = baseE && typeStrOf(*baseE).starts_with("mapping");
        if (!isMapping || !idxE) return llvm::ConstantInt::get(i256, 0);
        int baseSlot = -1;
        if (nodeType(*baseE) == "Identifier")
            if (auto r = baseE->getInteger("referencedDeclaration")) baseSlot = slotOfDecl(*r);
        if (baseSlot < 0) return llvm::ConstantInt::get(i256, 0);
        auto* ptr = emitMapElemPtrRaw(baseSlot, *idxE);
        if (!ptr) return nullptr;
        auto* load = builder_->CreateLoad(i256, ptr, "mload");
        tagStorage(load, baseSlot);
        // сузить к типу значения mapping (…=> bool/address)
        llvm::StringRef ts = typeStrOf(e);
        auto* target = typeFromStr(ts);
        return target->isIntegerTy(256) ? load : builder_->CreateTrunc(load, target, "trunc");
    }

    if (nt == "Conditional") {
        auto* c = e.getObject("condition")       ? emitExpr(*e.getObject("condition"))       : nullptr;
        auto* t = e.getObject("trueExpression")  ? emitExpr(*e.getObject("trueExpression"))  : nullptr;
        auto* f = e.getObject("falseExpression") ? emitExpr(*e.getObject("falseExpression")) : nullptr;
        if (!c || !t || !f) return nullptr;
        if (!c->getType()->isIntegerTy(1)) c = builder_->CreateICmpNE(c, llvm::ConstantInt::get(c->getType(), 0), "tobool");
        matchWidths(*builder_, t, f);
        if (t->getType() != f->getType()) return t;
        return builder_->CreateSelect(c, t, f, "cond");
    }

    if (nt == "TupleExpression") {
        // (x) — группировка; (a,b) — кортеж. эмитим компоненты ради эффектов, вернём первый
        const auto* comps = e.getArray("components");
        llvm::Value* first = nullptr;
        if (comps) for (const auto& c : *comps) {
            if (const auto* co = c.getAsObject()) {
                auto* v = emitExpr(*co);
                if (!first) first = v;
            }
        }
        return first ? first : llvm::ConstantInt::get(i256, 0);
    }

    // ElementaryTypeNameExpression / прочее — opaque
    return llvm::ConstantInt::get(i256, 0);
}

// присваивание как выражение (обычно внутри ExpressionStatement)
llvm::Value* AstJsonLowering::emitAssignment(const JObj& a) {
    const auto* lhs = a.getObject("leftHandSide");
    const auto* rhs = a.getObject("rightHandSide");
    if (!lhs || !rhs) return nullptr;
    auto* val = emitExpr(*rhs);
    if (!val) return nullptr;
    auto op = strField(a, "operator");
    if (op != "=") {
        // составное += -= *= /= : читаем текущее lvalue и комбинируем
        auto* cur = emitExpr(*lhs);
        if (!cur) return nullptr;
        matchWidths(*builder_, cur, val);
        if      (op == "+=") val = builder_->CreateAdd(cur, val, "addeq");
        else if (op == "-=") val = builder_->CreateSub(cur, val, "subeq");
        else if (op == "*=") val = builder_->CreateMul(cur, val, "muleq");
        else if (op == "/=") val = builder_->CreateUDiv(cur, val, "diveq");
        else                 val = builder_->CreateAdd(cur, val, "opeq");
    }
    storeToLValue(*lhs, val);
    return val;
}

// запись значения в lvalue: mapping-элемент / поле-ячейку / локаль / стейт-вар
void AstJsonLowering::storeToLValue(const JObj& lhs, llvm::Value* val) {
    auto* i256 = llvm::IntegerType::get(*ctx_, 256);
    auto nt = nodeType(lhs);

    if (nt == "IndexAccess") {
        const auto* baseE = lhs.getObject("baseExpression");
        const auto* idxE  = lhs.getObject("indexExpression");
        bool isMapping = baseE && typeStrOf(*baseE).starts_with("mapping");
        if (isMapping && idxE && nodeType(*baseE) == "Identifier") {
            int baseSlot = -1;
            if (auto r = baseE->getInteger("referencedDeclaration")) baseSlot = slotOfDecl(*r);
            if (baseSlot >= 0) {
                auto* ptr = emitMapElemPtrRaw(baseSlot, *idxE);
                if (!ptr) return;
                llvm::Value* wide = val->getType()->isIntegerTy(256) ? val
                                    : builder_->CreateZExt(val, i256, "zext");
                auto* st = builder_->CreateStore(wide, ptr);
                tagStorage(st, baseSlot);
                return;
            }
        }
        // массив/opaque, укоренён в storage → честный маркер непрозрачной записи
        if (rootsAtStorage(lhs)) builder_->CreateCall(intr_.opaque_store, {});
        return;
    }

    if (nt == "MemberAccess") {
        // поле struct с известным слотом (real/synthetic) — настоящая запись в storage.
        // критично для reentrancy: users[k].amount = 0 после вызова -> HIGH, а не opaque LOW
        if (auto cell = structFieldCell(lhs)) {
            llvm::Value* wide = val->getType()->isIntegerTy(256) ? val
                                : builder_->CreateZExt(val, i256, "zext");
            auto* st = builder_->CreateStore(wide, cell->ptr);
            tagStorage(st, cell->slot);
            return;
        }
        // поле не резолвится, но укоренено в storage -> честный маркер (LOW)
        if (rootsAtStorage(lhs)) builder_->CreateCall(intr_.opaque_store, {});
        return;
    }

    if (nt == "Identifier") {
        auto name = strField(lhs, "name");
        auto lit = locals_.find(name.str());
        if (lit != locals_.end()) {
            llvm::Value* cv = val;
            if (val->getType() != lit->second.type && val->getType()->isIntegerTy() &&
                lit->second.type->isIntegerTy()) {
                unsigned vb = val->getType()->getIntegerBitWidth();
                unsigned tb = lit->second.type->getIntegerBitWidth();
                cv = vb < tb ? builder_->CreateZExt(val, lit->second.type, "vext")
                             : (vb > tb ? builder_->CreateTrunc(val, lit->second.type, "vtrunc") : val);
            }
            builder_->CreateStore(cv, lit->second.alloca);
            return;
        }
        if (auto r = lhs.getInteger("referencedDeclaration")) {
            int slot = slotOfDecl(*r);
            if (slot >= 0) { emitStateStore(slot, val); return; }
        }
        return;
    }
    // TupleExpression LHS ((a,b)=…): rhs уже эмитирован — no-op
}

namespace {
llvm::StringRef jnt(const llvm::json::Object& n) {
    auto s = n.getString("nodeType"); return s ? *s : llvm::StringRef{};
}
// база lvalue → id декларации (Identifier / IndexAccess.base / MemberAccess.expr)
int64_t jlvalueBaseDecl(const llvm::json::Object& lv) {
    auto nt = jnt(lv);
    if (nt == "Identifier") { auto r = lv.getInteger("referencedDeclaration"); return r ? *r : 0; }
    if (nt == "IndexAccess")
        if (const auto* b = lv.getObject("baseExpression")) return jlvalueBaseDecl(*b);
    if (nt == "MemberAccess")
        if (const auto* e = lv.getObject("expression")) return jlvalueBaseDecl(*e);
    return 0;
}
// есть ли в поддереве запись в state-variable (Assignment / ++/-- по lvalue-базе-стейт-вару)
bool jwritesState(const llvm::json::Value& node,
                  const std::unordered_map<int64_t, const llvm::json::Object*>& idx, int depth = 0) {
    if (depth > 256) return false;
    if (const auto* o = node.getAsObject()) {
        auto nt = jnt(*o);
        const llvm::json::Object* lv = nullptr;
        if (nt == "Assignment") lv = o->getObject("leftHandSide");
        else if (nt == "UnaryOperation") {
            auto op = o->getString("operator");
            if (op && (*op == "++" || *op == "--")) lv = o->getObject("subExpression");
        }
        if (lv) {
            int64_t d = jlvalueBaseDecl(*lv);
            if (d) { auto it = idx.find(d);
                if (it != idx.end()) { auto sv = it->second->getBoolean("stateVariable");
                    if (sv && *sv) return true; } }
        }
        for (const auto& kv : *o) if (jwritesState(kv.second, idx, depth + 1)) return true;
    } else if (const auto* a = node.getAsArray()) {
        for (const auto& e : *a) if (jwritesState(e, idx, depth + 1)) return true;
    }
    return false;
}
}  // namespace

// резолвит obj.method() к конкретной функции (имплементер интерфейса или прямой контракт)
// и проверяет, что та ПИШЕТ состояние — для подтверждённого cross-contract эффекта (2b)
bool AstJsonLowering::calleeWritesState(const JObj& callee) {
    auto ref = callee.getInteger("referencedDeclaration");
    if (!ref) return false;
    const JObj* decl = nodeById(*ref);
    if (!decl) return false;
    const JObj* concrete = decl;
    if (!decl->getObject("body")) {              // интерфейс-прото → резолвим имплементера
        auto scope = decl->getInteger("scope");
        if (!scope || ambiguousImpl_.count(*scope)) return false;
        auto it = implementerOf_.find(*scope);
        if (it == implementerOf_.end()) return false;
        llvm::StringRef method = strField(*decl, "name");
        concrete = nullptr;
        if (const auto* nodes = it->second->getArray("nodes"))
            for (const auto& n : *nodes)
                if (const auto* f = n.getAsObject())
                    if (nodeType(*f) == "FunctionDefinition" && strField(*f, "name") == method &&
                        f->getBoolean("implemented").value_or(false)) { concrete = f; break; }
        if (!concrete) return false;
    }
    const JObj* body = concrete->getObject("body");
    if (!body) return false;
    for (const auto& kv : *body)
        if (jwritesState(kv.second, idIndex_)) return true;
    return false;
}

// вызов функции: внешний low-level / .transfer / внутренний / typeConversion / builtin
llvm::Value* AstJsonLowering::emitFunctionCall(const JObj& c) {
    auto* i256 = llvm::IntegerType::get(*ctx_, 256);
    auto* i160 = llvm::IntegerType::get(*ctx_, 160);
    auto kind = strField(c, "kind");
    const auto* callee = c.getObject("expression");
    const auto* args = c.getArray("arguments");

    // приведение типа: address(x)/uint256(x)/payable(x)/IERC20(x)
    if (kind == "typeConversion") {
        llvm::Value* v = (args && !args->empty() && (*args)[0].getAsObject())
                         ? emitExpr(*(*args)[0].getAsObject()) : llvm::ConstantInt::get(i256, 0);
        if (!v) return nullptr;
        auto* target = typeFromStr(typeStrOf(c));
        if (v->getType() != target && v->getType()->isIntegerTy() && target->isIntegerTy()) {
            unsigned vb = v->getType()->getIntegerBitWidth(), tb = target->getIntegerBitWidth();
            v = vb < tb ? builder_->CreateZExt(v, target, "cext")
                        : (vb > tb ? builder_->CreateTrunc(v, target, "ctrunc") : v);
        }
        return v;
    }
    if (!callee) return llvm::ConstantInt::get(i256, 0);

    // помощник: привести к i160 (адрес получателя) / i256 (значение)
    auto toAddr = [&](llvm::Value* v) -> llvm::Value* {
        if (!v) return llvm::ConstantInt::get(i160, 0);
        if (v->getType()->isIntegerTy(160)) return v;
        if (!v->getType()->isIntegerTy()) return llvm::ConstantInt::get(i160, 0);
        return v->getType()->getIntegerBitWidth() < 160
               ? builder_->CreateZExt(v, i160, "aext") : builder_->CreateTrunc(v, i160, "atrunc");
    };
    auto toWei = [&](llvm::Value* v) -> llvm::Value* {
        if (!v) return llvm::ConstantInt::get(i256, 0);
        if (v->getType()->isIntegerTy(256)) return v;
        return builder_->CreateZExt(v, i256, "vext");
    };

    // внешний вызов с опциями: addr.call{value: v}("") / .delegatecall / .staticcall
    if (nodeType(*callee) == "FunctionCallOptions") {
        const auto* inner = callee->getObject("expression");
        if (inner && nodeType(*inner) == "MemberAccess") {
            auto m = strField(*inner, "memberName");
            if (m == "call" || m == "delegatecall" || m == "staticcall") {
                llvm::Value* target = inner->getObject("expression")
                                      ? emitExpr(*inner->getObject("expression")) : nullptr;
                // value из options по индексу names=="value"
                llvm::Value* value = llvm::ConstantInt::get(i256, 0);
                const auto* names = callee->getArray("names");
                const auto* opts  = callee->getArray("options");
                if (names && opts)
                    for (size_t i = 0; i < names->size() && i < opts->size(); ++i)
                        if ((*names)[i].getAsString() == "value" && (*opts)[i].getAsObject())
                            value = emitExpr(*(*opts)[i].getAsObject());
                auto* call = builder_->CreateCall(intr_.external_call,
                                                  {toAddr(target), toWei(value)}, "extcall");
                const char* k = m == "delegatecall" ? "delegatecall"
                                : m == "staticcall" ? "staticcall" : "call";
                tagExternalCall(call, k);
                // низкоуровневый .call(data): calldata может быть attacker-controlled (bytes-аргумент)
                // — отличаем от типизированного метода token.transfer(...) с фиксированным селектором
                call->setMetadata("scsa.call.raw", llvm::MDNode::get(*ctx_, {}));
                return call;
            }
        }
        // иные опции — эмитим внутреннее выражение
        if (const auto* inner2 = callee->getObject("expression")) return emitExpr(*inner2);
        return llvm::ConstantInt::get(i256, 0);
    }

    if (nodeType(*callee) == "MemberAccess") {
        auto m = strField(*callee, "memberName");
        // .call("")/.delegatecall без value
        if (m == "call" || m == "delegatecall" || m == "staticcall") {
            llvm::Value* target = callee->getObject("expression")
                                  ? emitExpr(*callee->getObject("expression")) : nullptr;
            auto* call = builder_->CreateCall(intr_.external_call,
                                              {toAddr(target), llvm::ConstantInt::get(i256, 0)}, "extcall");
            const char* k = m == "delegatecall" ? "delegatecall"
                            : m == "staticcall" ? "staticcall" : "call";
            tagExternalCall(call, k);
            // низкоуровневый .call(data): calldata может быть attacker-controlled (bytes-аргумент)
            call->setMetadata("scsa.call.raw", llvm::MDNode::get(*ctx_, {}));
            return call;
        }
        // .transfer(x) / .send(x) на address(payable) — нативный ETH-перевод с value=x.
        // ВАЖНО: только для address-получателя с РОВНО одним аргументом. token.transfer(to, amt)
        // на контракте/интерфейсе — это ERC20-метод (2 арг), НЕ нативный перевод: иначе args[0]
        // (получатель) ошибочно берётся как value → его storage-адрес попадает в value-трассу и
        // даёт мнимый слот (MOG clearStuckToken → ложная cross-function reentrancy, проверено на
        // mainnet). такие падают в общий путь внешнего метода ниже (external_call, value=0)
        if (m == "transfer" || m == "send") {
            const auto* rexpr = callee->getObject("expression");
            llvm::StringRef rt = rexpr ? typeStrOf(*rexpr) : llvm::StringRef{};
            if (!rt.starts_with("contract ") && args && args->size() == 1) {
                llvm::Value* target = rexpr ? emitExpr(*rexpr) : nullptr;
                llvm::Value* value  = (*args)[0].getAsObject()
                                      ? emitExpr(*(*args)[0].getAsObject()) : nullptr;
                auto* call = builder_->CreateCall(intr_.external_call,
                                                  {toAddr(target), toWei(value)}, "extcall");
                tagExternalCall(call, "call");
                // .send возвращает отбрасываемый bool (SWC-104 применим); .transfer авто-реверит
                if (m == "send")
                    call->setMetadata("scsa.call.raw", llvm::MDNode::get(*ctx_, {}));
                return call;
            }
            // token.transfer(to, amount) — ERC20-метод: обрабатывается общим путём ниже
        }
        // внешний метод obj.method(args): если получатель — контракт/интерфейс и метод
        // МЕНЯЕТ состояние, это ВНЕШНИЙ вызов (control уходит наружу, возможен reentrant
        // callback). ловит transferFrom/safeTransfer/ERC777 send/любой interface-метод —
        // ключевые reentrancy-векторы, ранее невидимые (был opaque 0). i1-результат годится
        // и как условие require (чинит require(externalcall)->br i1 false, удалявший путь).
        // view/pure — staticcall, реентрантно менять состояние не может -> оставляем opaque
        {
            const auto* recv = callee->getObject("expression");
            llvm::StringRef recvType = recv ? typeStrOf(*recv) : llvm::StringRef{};
            llvm::StringRef baseName = (recv && nodeType(*recv) == "Identifier")
                                       ? strField(*recv, "name") : llvm::StringRef{};
            llvm::StringRef fnType = typeStrOf(*callee);
            bool readOnly = fnType.contains("view") || fnType.contains("pure");
            // однозначные DEX-spot-price источники: значение манипулируемо flash-loan-ом.
            // маркер @scsa.oracle_read (символьное i256) — детектор price-manipulation ловит,
            // когда оно доходит до отправки средств. НЕ включаем Chainlink latestAnswer
            // (обычно безопасен) и balanceOf (слишком часто → FP)
            if (recvType.starts_with("contract ") &&
                (m == "getReserves" || m == "slot0" || m == "getAmountsOut" ||
                 m == "getAmountOut" || m == "getAmountsIn" || m == "getAmountIn")) {
                if (recv) emitExpr(*recv);
                if (args) for (const auto& a : *args) if (const auto* ao = a.getAsObject()) emitExpr(*ao);
                auto orFn = module_->getOrInsertFunction(
                    "scsa.oracle_read", llvm::FunctionType::get(i256, {}, false));
                return builder_->CreateCall(orFn, {}, "oracle");
            }
            // Chainlink price-feed чтения (AggregatorV3Interface): НЕ flash-loan-манипулируемы
            // как spot-DEX (OraclePriceDetector их пропускает по метке), но их часто используют
            // БЕЗ проверки свежести (updatedAt vs block.timestamp) — маркер для OracleStalenessDetector.
            // частый Ярус-2 класс (LoopFi deprecated answeredInRound, Peapods circuit-breaker)
            if (recvType.starts_with("contract ") &&
                (m == "latestRoundData" || m == "latestAnswer" || m == "getRoundData" ||
                 m == "latestRound")) {
                if (recv) emitExpr(*recv);
                if (args) for (const auto& a : *args) if (const auto* ao = a.getAsObject()) emitExpr(*ao);
                auto orFn = module_->getOrInsertFunction(
                    "scsa.oracle_read", llvm::FunctionType::get(i256, {}, false));
                auto* call = builder_->CreateCall(orFn, {}, "chainlink");
                call->setMetadata("scsa.oracle.chainlink", llvm::MDNode::get(*ctx_, {}));
                return call;
            }
            bool externalStateChanging = recvType.starts_with("contract ") &&
                                         baseName != "this" && baseName != "super" && !readOnly;
            if (externalStateChanging) {
                llvm::Value* target = recv ? emitExpr(*recv) : nullptr;
                if (args) for (const auto& a : *args) if (const auto* ao = a.getAsObject()) emitExpr(*ao);
                auto* call = builder_->CreateCall(intr_.external_call,
                                                  {toAddr(target), llvm::ConstantInt::get(i256, 0)}, "extcall");
                tagExternalCall(call, "call");
                // своп на DEX-РОУТЕРЕ (named): маркер для SlippageDetector (MEV-сэндвич при слабой
                // slippage). НЕ bare `swap` — контракт-`.swap()` часто защищённый свопер (FP)
                if (isRouterSwap(m))
                    call->setMetadata("scsa.swap", llvm::MDNode::get(*ctx_, {}));
                // подтверждённый cross-contract эффект: резолвим метод к имплементеру и
                // помечаем, если тот реально пишет состояние (детектор → HIGH вместо MEDIUM)
                if (calleeWritesState(*callee))
                    call->setMetadata("scsa.xcall.effect", llvm::MDNode::get(*ctx_,
                        {llvm::ConstantAsMetadata::get(llvm::ConstantInt::getTrue(*ctx_))}));
                return call;
            }
        }
        // library-метод/view/не-контракт-получатель — opaque, но эмитим аргументы ради эффектов
        if (args) for (const auto& a : *args) if (const auto* ao = a.getAsObject()) emitExpr(*ao);
        // DEX-своп через библиотеку-helper (DexSwap.swap) — иначе невидим в IR: маркер @scsa.swap,
        // чтобы SlippageDetector ловил reinvest-адаптеры (не только named-роутеры на контракте)
        if (isSwapMethod(m)) {
            auto swFn = module_->getOrInsertFunction("scsa.swap", llvm::FunctionType::get(i256, {}, false));
            return builder_->CreateCall(swFn, {}, "swap");
        }
        return llvm::ConstantInt::get(i256, 0);
    }

    if (nodeType(*callee) == "Identifier") {
        auto name = strField(*callee, "name");
        if (name == "require" || name == "assert") {
            if (args && !args->empty() && (*args)[0].getAsObject()) emitRequire(*(*args)[0].getAsObject());
            return llvm::ConstantInt::get(i256, 0);
        }
        if (name == "revert") {
            builder_->CreateCall(intr_.revert, {});
            builder_->CreateUnreachable();
            return llvm::ConstantInt::get(i256, 0);
        }
        if (name == "selfdestruct" || name == "suicide") {
            llvm::Value* r = (args && !args->empty() && (*args)[0].getAsObject())
                             ? emitExpr(*(*args)[0].getAsObject()) : nullptr;
            builder_->CreateCall(intr_.self_destruct, {toAddr(r)});
            return llvm::ConstantInt::get(i256, 0);
        }
        // внутренний вызов функции контракта — реальный call (межпроцедурный анализ)
        auto fit = funcByName_.find(name.str());
        if (fit != funcByName_.end()) {
            llvm::Function* fn = fit->second;
            unsigned n = fn->arg_size();
            std::vector<llvm::Value*> callArgs(n, nullptr);
            unsigned k = 0;
            if (args) for (const auto& a : *args) {
                if (const auto* ao = a.getAsObject()) {
                    auto* v = emitExpr(*ao);
                    if (v && k < n) {
                        auto* pty = fn->getArg(k)->getType();
                        if (v->getType() != pty && v->getType()->isIntegerTy() && pty->isIntegerTy()) {
                            unsigned vb = v->getType()->getIntegerBitWidth(), pb = pty->getIntegerBitWidth();
                            v = vb < pb ? builder_->CreateZExt(v, pty) : (vb > pb ? builder_->CreateTrunc(v, pty) : v);
                        }
                        callArgs[k] = v;
                    }
                }
                ++k;
            }
            for (unsigned j = 0; j < n; ++j)
                if (!callArgs[j]) callArgs[j] = llvm::Constant::getNullValue(fn->getArg(j)->getType());
            auto* call = builder_->CreateCall(fn, callArgs);
            return fn->getReturnType()->isVoidTy()
                   ? static_cast<llvm::Value*>(llvm::ConstantInt::get(i256, 0))
                   : static_cast<llvm::Value*>(call);
        }
        // builtin (keccak256/abi.*/…) — opaque, эмитим аргументы
        if (args) for (const auto& a : *args) if (const auto* ao = a.getAsObject()) emitExpr(*ao);
        return llvm::ConstantInt::get(i256, 0);
    }

    if (args) for (const auto& a : *args) if (const auto* ao = a.getAsObject()) emitExpr(*ao);
    return llvm::ConstantInt::get(i256, 0);
}

// require(cond)/assert(cond) -> br cond, ok, fail; fail: revert + unreachable
void AstJsonLowering::emitRequire(const JObj& condExpr) {
    auto* cond = emitExpr(condExpr);
    if (!cond) return;
    if (!cond->getType()->isIntegerTy(1))
        cond = builder_->CreateICmpNE(cond, llvm::ConstantInt::get(cond->getType(), 0), "tobool");
    // условие свернулось в константу — как правило это OPAQUE-значение (нераспознанный
    // вызов вернул 0), а НЕ настоящий require(false). не эмитим br на константу: иначе
    // require(opaqueBool) -> require(false) удалил бы весь путь с его эффектами (ложный FN).
    // считаем такой require пройденным (консервативно продолжаем анализ пути)
    if (llvm::isa<llvm::Constant>(cond)) return;
    auto* okBB   = llvm::BasicBlock::Create(*ctx_, "require.ok",   currentFn_);
    auto* failBB = llvm::BasicBlock::Create(*ctx_, "require.fail", currentFn_);
    builder_->CreateCondBr(cond, okBB, failBB);
    builder_->SetInsertPoint(failBB);
    builder_->CreateCall(intr_.revert, {});
    builder_->CreateUnreachable();
    builder_->SetInsertPoint(okBB);
}

// -------- инструкции --------

void AstJsonLowering::emitBlockLike(const JVal* node) {
    if (!node) return;
    const auto* o = node->getAsObject();
    if (!o) return;
    if (nodeType(*o) == "Block" || nodeType(*o) == "UncheckedBlock") {
        if (const auto* sts = o->getArray("statements"))
            for (const auto& s : *sts)
                if (const auto* so = s.getAsObject()) emitStmt(*so);
    } else {
        emitStmt(*o);
    }
}

void AstJsonLowering::emitStmt(const JObj& s) {
    if (builder_->GetInsertBlock()->getTerminator()) return;
    setDebugLocFromSrc(s);
    auto nt = nodeType(s);

    if (nt == "Block" || nt == "UncheckedBlock") {
        if (const auto* sts = s.getArray("statements"))
            for (const auto& st : *sts)
                if (const auto* so = st.getAsObject()) emitStmt(*so);
        return;
    }
    if (nt == "ExpressionStatement") {
        if (const auto* ex = s.getObject("expression")) emitExpr(*ex);
        return;
    }
    if (nt == "VariableDeclarationStatement") {
        const auto* decls = s.getArray("declarations");
        const auto* initV = s.getObject("initialValue");
        // единственная непустая декларация
        const JObj* only = nullptr;
        int nonNull = 0;
        if (decls) for (const auto& d : *decls)
            if (const auto* dobj = d.getAsObject()) { ++nonNull; if (!only) only = dobj; }
        // storage-ссылка: `T storage x = map[k]`
        if (only && nonNull == 1 && strField(*only, "storageLocation") == "storage" &&
            initV && nodeType(*initV) == "IndexAccess") {
            const auto* baseE = initV->getObject("baseExpression");
            const auto* idxE  = initV->getObject("indexExpression");
            if (baseE && idxE && typeStrOf(*baseE).starts_with("mapping") &&
                nodeType(*baseE) == "Identifier") {
                int baseSlot = -1;
                if (auto r = baseE->getInteger("referencedDeclaration")) baseSlot = slotOfDecl(*r);
                if (baseSlot >= 0) {
                    storageAliases_[strField(*only, "name").str()] = StorageAliasJ{baseSlot, idxE};
                    // локаль-заглушка, чтобы bare-обращения не падали
                    auto* ty = llvm::IntegerType::get(*ctx_, 256);
                    auto* alc = builder_->CreateAlloca(ty, nullptr, strField(*only, "name"));
                    builder_->CreateStore(llvm::ConstantInt::get(ty, 0), alc);
                    locals_[strField(*only, "name").str()] = LocalSlot{alc, ty};
                    return;
                }
            }
        }
        llvm::Value* val = initV ? emitExpr(*initV) : nullptr;
        // создаём локали; скалярное значение кладём в первую непустую декларацию
        bool bound = false;
        if (decls) for (const auto& d : *decls) {
            const auto* dobj = d.getAsObject();
            if (!dobj) continue;
            auto* ty = typeFromStr(typeStrOf(*dobj));
            auto* alc = builder_->CreateAlloca(ty, nullptr, strField(*dobj, "name"));
            llvm::Value* init = llvm::Constant::getNullValue(ty);
            if (!bound && val) {
                init = val;
                if (init->getType() != ty && init->getType()->isIntegerTy() && ty->isIntegerTy()) {
                    unsigned vb = init->getType()->getIntegerBitWidth(), tb = ty->getIntegerBitWidth();
                    init = vb < tb ? builder_->CreateZExt(init, ty, "vext")
                                   : (vb > tb ? builder_->CreateTrunc(init, ty, "vtrunc") : init);
                }
                bound = true;
            }
            builder_->CreateStore(init, alc);
            locals_[strField(*dobj, "name").str()] = LocalSlot{alc, ty};
        }
        return;
    }
    if (nt == "IfStatement") {
        const auto* condN = s.getObject("condition");
        auto* cond = condN ? emitExpr(*condN) : nullptr;
        if (!cond) return;
        if (!cond->getType()->isIntegerTy(1))
            cond = builder_->CreateICmpNE(cond, llvm::ConstantInt::get(cond->getType(), 0), "tobool");
        auto* thenBB  = llvm::BasicBlock::Create(*ctx_, "if.then",  currentFn_);
        auto* elseBB  = llvm::BasicBlock::Create(*ctx_, "if.else",  currentFn_);
        auto* mergeBB = llvm::BasicBlock::Create(*ctx_, "if.merge", currentFn_);
        builder_->CreateCondBr(cond, thenBB, elseBB);
        builder_->SetInsertPoint(thenBB);
        emitBlockLike(s.get("trueBody"));
        if (!builder_->GetInsertBlock()->getTerminator()) builder_->CreateBr(mergeBB);
        builder_->SetInsertPoint(elseBB);
        emitBlockLike(s.get("falseBody"));
        if (!builder_->GetInsertBlock()->getTerminator()) builder_->CreateBr(mergeBB);
        if (mergeBB->hasNPredecessors(0)) mergeBB->eraseFromParent();
        else builder_->SetInsertPoint(mergeBB);
        return;
    }
    if (nt == "Return") {
        const auto* ex = s.getObject("expression");
        if (ex) {
            auto* v = emitExpr(*ex);
            if (!v) return;
            auto* rty = currentFn_->getReturnType();
            if (rty->isVoidTy()) { builder_->CreateRetVoid(); return; }
            if (v->getType() != rty && v->getType()->isIntegerTy() && rty->isIntegerTy()) {
                unsigned vb = v->getType()->getIntegerBitWidth(), rb = rty->getIntegerBitWidth();
                v = vb < rb ? builder_->CreateZExt(v, rty, "retext") : builder_->CreateTrunc(v, rty, "rettrunc");
            }
            builder_->CreateRet(v);
        } else {
            if (currentFn_->getReturnType()->isVoidTy()) builder_->CreateRetVoid();
            else builder_->CreateRet(llvm::Constant::getNullValue(currentFn_->getReturnType()));
        }
        return;
    }
    if (nt == "RevertStatement") {   // 0.8: revert CustomError()
        builder_->CreateCall(intr_.revert, {});
        builder_->CreateUnreachable();
        return;
    }
    if (nt == "ForStatement" || nt == "WhileStatement" || nt == "DoWhileStatement") {
        // приближение: тело один раз (эффекты видны детектору), без обратной дуги
        if (const auto* init = s.getObject("initializationExpression")) emitStmt(*init);
        emitBlockLike(s.get("body"));
        return;
    }
    if (nt == "PlaceholderStatement" || nt == "EmitStatement" ||
        nt == "Continue" || nt == "Break" || nt == "InlineAssembly")
        return;   // _; эмитится через emitWithModifiers; события/asm — opaque
    // прочее — no-op
}

// -------- функции / модификаторы --------

std::vector<const JObj*> AstJsonLowering::collectFnModifiers(const JObj& fn) {
    std::vector<const JObj*> mods;
    const auto* invs = fn.getArray("modifiers");
    if (!invs) return mods;
    for (const auto& inv : *invs) {
        const auto* io = inv.getAsObject();
        if (!io) continue;
        const auto* mn = io->getObject("modifierName");
        if (!mn) continue;
        auto ref = mn->getInteger("referencedDeclaration");
        if (!ref) continue;
        const auto* def = nodeById(*ref);
        if (def && nodeType(*def) == "ModifierDefinition") mods.push_back(def);
        // иначе это вызов конструктора базы (Ownable()) — не модификатор, пропускаем
    }
    return mods;
}

void AstJsonLowering::emitWithModifiers(const JObj& fn, size_t modIdx) {
    if (modIdx >= curMods_.size()) {
        if (const auto* body = fn.getObject("body"))
            if (const auto* sts = body->getArray("statements"))
                for (const auto& s : *sts)
                    if (const auto* so = s.getAsObject()) emitStmt(*so);
        return;
    }
    const JObj* md = curMods_[modIdx];
    // параметры модификатора связываем нулём (аргументы вызова отбрасываем)
    if (const auto* pl = md->getObject("parameters"))
        if (const auto* ps = pl->getArray("parameters"))
            for (const auto& p : *ps)
                if (const auto* po = p.getAsObject()) {
                    auto name = strField(*po, "name");
                    if (name.empty() || locals_.count(name.str())) continue;
                    auto* ty = typeFromStr(typeStrOf(*po));
                    auto* alc = builder_->CreateAlloca(ty, nullptr, name);
                    builder_->CreateStore(llvm::Constant::getNullValue(ty), alc);
                    locals_[name.str()] = LocalSlot{alc, ty};
                }
    bool placeholderSeen = false;
    if (const auto* body = md->getObject("body"))
        if (const auto* sts = body->getArray("statements"))
            for (const auto& s : *sts) {
                if (builder_->GetInsertBlock()->getTerminator()) break;
                const auto* so = s.getAsObject();
                if (!so) continue;
                if (nodeType(*so) == "PlaceholderStatement") {
                    placeholderSeen = true;
                    emitWithModifiers(fn, modIdx + 1);
                } else {
                    emitStmt(*so);
                }
            }
    if (!placeholderSeen && !builder_->GetInsertBlock()->getTerminator())
        emitWithModifiers(fn, modIdx + 1);
}

// имя функции для IR: name, либо kind для fallback/receive
static std::string fnIrName(const JObj& fn) {
    auto kind = fn.getString("kind");
    if (kind && (*kind == "fallback")) return "fallback";
    if (kind && (*kind == "receive"))  return "receive";
    auto name = fn.getString("name");
    return name && !name->empty() ? name->str() : "anon";
}

llvm::Function* AstJsonLowering::emitFunctionProto(const JObj& fn) {
    std::vector<llvm::Type*> paramTys;
    if (const auto* pl = fn.getObject("parameters"))
        if (const auto* ps = pl->getArray("parameters"))
            for (const auto& p : *ps)
                if (const auto* po = p.getAsObject()) paramTys.push_back(typeFromStr(typeStrOf(*po)));
    llvm::Type* retTy = llvm::Type::getVoidTy(*ctx_);
    if (const auto* rl = fn.getObject("returnParameters"))
        if (const auto* rs = rl->getArray("parameters"))
            if (!rs->empty())
                if (const auto* ro = (*rs)[0].getAsObject()) retTy = typeFromStr(typeStrOf(*ro));
    auto* ft = llvm::FunctionType::get(retTy, paramTys, false);
    auto* func = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, fnIrName(fn), module_.get());

    // !scsa.abi — реальные ABI-типы параметров (из solc-AST) для верного селектора в PoC.
    // downstream (PocGenerator) читает их через trace::functionParamAbiTypes
    std::vector<llvm::Metadata*> abi;
    if (const auto* pl = fn.getObject("parameters"))
        if (const auto* ps = pl->getArray("parameters"))
            for (const auto& p : *ps)
                if (const auto* po = p.getAsObject())
                    abi.push_back(llvm::MDString::get(*ctx_, abiTypeOf(typeStrOf(*po))));
    func->setMetadata("scsa.abi", llvm::MDNode::get(*ctx_, abi));
    // видимость: internal/private-функции — НЕ точки входа атаки (reentrancy-детектор
    // их пропускает; вход только через public/external, где и стоит guard)
    auto vis = fn.getString("visibility");
    if (vis && (*vis == "internal" || *vis == "private"))
        func->setMetadata("scsa.internal", llvm::MDNode::get(*ctx_, {}));
    return func;
}

void AstJsonLowering::emitFunctionBody(const JObj& fn, llvm::Function* func) {
    locals_.clear();
    storageAliases_.clear();
    currentFn_ = func;
    curMods_ = collectFnModifiers(fn);

    // имена модификаторов функции → метаданные: детекторы отличают guard-модификаторы
    // (initializer/onlyProxy/…) — контекстный/разовый guard, не access-control-баг
    if (!curMods_.empty()) {
        std::vector<llvm::Metadata*> mnames;
        for (const auto* md : curMods_)
            mnames.push_back(llvm::MDString::get(*ctx_, strField(*md, "name")));
        func->setMetadata("scsa.modifiers", llvm::MDNode::get(*ctx_, mnames));
    }

    unsigned line = 1;
    if (auto src = fn.getString("src")) line = lineOfSrc(*src);
    auto* subTy = dib_->createSubroutineType(dib_->getOrCreateTypeArray({}));
    diSP_ = dib_->createFunction(diFile_, fnIrName(fn), fnIrName(fn), diFile_, line, subTy,
                                 line, llvm::DINode::FlagZero, llvm::DISubprogram::SPFlagDefinition);
    func->setSubprogram(diSP_);

    auto* entry = llvm::BasicBlock::Create(*ctx_, "entry", func);
    builder_->SetInsertPoint(entry);
    setDebugLocFromSrc(fn);

    // alloca+store параметров (единая модель доступа)
    auto argIt = func->arg_begin();
    if (const auto* pl = fn.getObject("parameters"))
        if (const auto* ps = pl->getArray("parameters"))
            for (const auto& p : *ps) {
                const auto* po = p.getAsObject();
                if (!po) { ++argIt; continue; }
                auto* ty = typeFromStr(typeStrOf(*po));
                auto* alc = builder_->CreateAlloca(ty, nullptr, strField(*po, "name"));
                builder_->CreateStore(&*argIt, alc);
                auto nm = strField(*po, "name");
                if (!nm.empty()) locals_[nm.str()] = LocalSlot{alc, ty};
                ++argIt;
            }

    emitWithModifiers(fn, 0);

    if (!builder_->GetInsertBlock()->getTerminator()) {
        if (func->getReturnType()->isVoidTy()) builder_->CreateRetVoid();
        else builder_->CreateRet(llvm::Constant::getNullValue(func->getReturnType()));
    }
    currentFn_ = nullptr;
    diSP_ = nullptr;
    builder_->SetCurrentDebugLocation(llvm::DebugLoc());
}

// собрать функции+модификаторы контракта с учётом наследования (most-derived wins)
void AstJsonLowering::collectInherited(const JObj& contract,
                                       std::vector<const JObj*>& funcs,
                                       std::vector<const JObj*>& mods) {
    const auto* lin = contract.getArray("linearizedBaseContracts");
    std::unordered_map<std::string, bool> seenFn, seenMod;
    auto visit = [&](const JObj& c) {
        if (const auto* nodes = c.getArray("nodes"))
            for (const auto& n : *nodes) {
                const auto* no = n.getAsObject();
                if (!no) continue;
                auto t = nodeType(*no);
                if (t == "FunctionDefinition") {
                    auto kind = strField(*no, "kind");
                    if (kind == "constructor") continue;   // конструктор не анализируем (не переиспользуемая точка входа)
                    bool impl = no->getBoolean("implemented").value_or(true);
                    if (!impl) continue;                   // прототип без тела
                    std::string sig = fnIrName(*no) + ":" + typeStrOf(*no).str();
                    if (seenFn[sig]) continue;
                    seenFn[sig] = true;
                    funcs.push_back(no);
                } else if (t == "ModifierDefinition") {
                    std::string nm = strField(*no, "name").str();
                    if (seenMod[nm]) continue;
                    seenMod[nm] = true;
                    mods.push_back(no);
                }
            }
    };
    if (lin) {
        for (const auto& id : *lin)
            if (auto i = id.getAsInteger())
                if (const auto* c = nodeById(*i)) visit(*c);
    } else {
        visit(contract);   // без наследования
    }
}

void AstJsonLowering::lowerContract(const JObj& contract, const std::string& unit) {
    std::string name = strField(contract, "name").str();
    unit_ = unit;   // файл, где определён контракт — для storageLayout ниже

    // новый контекст-владелец на контракт (переживёт все модули в modules_)
    contexts_.push_back(std::make_unique<llvm::LLVMContext>());
    ctx_     = contexts_.back().get();
    module_  = std::make_unique<llvm::Module>(name, *ctx_);
    builder_ = std::make_unique<llvm::IRBuilder<>>(*ctx_);
    module_->setDataLayout("e-p1:256:256-p2:256:256-p3:256:256");
    intr_ = declareEvmIntrinsics(*module_);
    initDebugInfo(name);

    // слоты стейт-вар из storageLayout контракта (astId -> slot, + typeString из декларации)
    slotById_.clear();
    typeStrById_.clear();
    memberSlotById_.clear();
    synthSlot_.clear();
    int maxSlot = 0;
    if (const auto* contracts = root_->getObject("contracts"))
        if (const auto* byUnit = contracts->getObject(unit_))
            if (const auto* cinfo = byUnit->getObject(name))
                if (const auto* sl = cinfo->getObject("storageLayout")) {
                    if (const auto* storage = sl->getArray("storage"))
                        for (const auto& ent : *storage) {
                            const auto* eo = ent.getAsObject();
                            if (!eo) continue;
                            auto astId = eo->getInteger("astId");
                            auto slotStr = eo->getString("slot");
                            if (!astId || !slotStr) continue;
                            long slot = 0; slotStr->getAsInteger(10, slot);
                            slotById_[*astId] = static_cast<int>(slot);
                            maxSlot = std::max(maxSlot, static_cast<int>(slot));
                            if (const auto* decl = nodeById(*astId))
                                typeStrById_[*astId] = typeStrOf(*decl).str();
                        }
                    // offset поля struct: types[*].members[].astId -> slot внутри struct
                    if (const auto* types = sl->getObject("types"))
                        for (const auto& kv : *types)
                            if (const auto* to = kv.second.getAsObject())
                                if (const auto* members = to->getArray("members"))
                                    for (const auto& m : *members) {
                                        const auto* mo = m.getAsObject();
                                        if (!mo) continue;
                                        auto mid = mo->getInteger("astId");
                                        auto mslot = mo->getString("slot");
                                        if (!mid || !mslot) continue;
                                        long off = 0; mslot->getAsInteger(10, off);
                                        memberSlotById_[*mid] = static_cast<int>(off);
                                    }
                }
    // синтетические слоты полей mapping-of-struct — с запасом над реальными (без коллизий,
    // покрывает хвосты struct-переменных; массив storage в KLEE остаётся компактным)
    synthNext_ = maxSlot + 256;

    // immutable-переменные НЕ попадают в storageLayout (вшиты в байткод), поэтому раньше их чтение
    // разрешалось в константу 0 → терялся guard `msg.sender != owner` при `immutable owner` (частый
    // газо-приём в свежих 0.8 → массовый ложный access-control). моделируем immutable как
    // синтетический storage-слот: чтение = load addrspace(1), детектор видит сравнение с owner-слотом.
    // запись безопасна — immutable пишется только в конструкторе, а тот не анализируется (kind==constructor)
    if (const auto* lin = contract.getArray("linearizedBaseContracts"))
        for (const auto& b : *lin)
            if (auto bid = b.getAsInteger())
                if (const auto* bc = nodeById(*bid))
                    if (const auto* bnodes = bc->getArray("nodes"))
                        for (const auto& nd : *bnodes) {
                            const auto* no = nd.getAsObject();
                            if (!no || nodeType(*no) != "VariableDeclaration") continue;
                            auto sv = no->getBoolean("stateVariable");
                            if (!sv || !*sv || strField(*no, "mutability") != "immutable") continue;
                            auto idOpt = no->getInteger("id");
                            if (!idOpt || slotById_.count(*idOpt)) continue;
                            slotById_[*idOpt] = synthNext_++;
                            typeStrById_[*idOpt] = typeStrOf(*no).str();
                        }

    std::vector<const JObj*> funcs, mods;
    collectInherited(contract, funcs, mods);

    modifierByName_.clear();
    for (const auto* m : mods) modifierByName_[strField(*m, "name").str()] = m;

    // два прохода: прототипы, затем тела (внутренние вызовы видят функции ниже)
    funcByName_.clear();
    for (const auto* f : funcs) funcByName_[fnIrName(*f)] = emitFunctionProto(*f);
    for (const auto* f : funcs) emitFunctionBody(*f, funcByName_[fnIrName(*f)]);

    dib_->finalize();
    if (llvm::verifyModule(*module_, nullptr)) {
        diag_.error({}, "internal: solc-lowered module failed verification (" + name + ")");
        return;
    }
    modules_.push_back(std::move(module_));
}

std::vector<std::unique_ptr<llvm::Module>>
AstJsonLowering::lowerAll(const llvm::json::Value& root, const std::string& primaryUnit,
                          const std::vector<std::pair<std::string, std::string>>& sources,
                          const std::string& targetContract) {
    modules_.clear();
    root_ = root.getAsObject();
    if (!root_) { diag_.error({}, "solc: пустой JSON"); return {}; }

    std::unordered_map<std::string, std::string> contentByPath;
    for (const auto& [p, c] : sources) contentByPath[p] = c;

    const auto* srcs = root_->getObject("sources");
    if (!srcs) { diag_.error({}, "solc: нет sources в выводе"); return {}; }

    // ВСЕ файлы: idIndex_ по всему проекту (базы OZ в других файлах) + line-таблицы по fileIndex
    idIndex_.clear();
    fileLineTables_.clear();
    std::vector<std::pair<std::string, const JObj*>> unitAsts;   // (путь, ast-объект)
    for (const auto& kv : *srcs) {
        const auto* so = kv.second.getAsObject();
        if (!so) continue;
        if (auto id = so->getInteger("id")) {
            auto it = contentByPath.find(kv.first.str());
            if (it != contentByPath.end())
                fileLineTables_[static_cast<int>(*id)] = buildLineStarts(it->second);
        }
        if (const JVal* ast = so->get("ast")) {
            driver::buildIdIndex(*ast, idIndex_);
            if (const auto* ao = ast->getAsObject()) unitAsts.emplace_back(kv.first.str(), ao);
        }
    }
    if (unitAsts.empty()) { diag_.error({}, "solc: нет AST"); return {}; }

    // резолв interface→имплементер (для подтверждённого cross-contract эффекта, проход 2b):
    // контракт-kind C реализует каждый свой базовый интерфейс из linearizedBaseContracts.
    // если интерфейс реализует >1 контракт — неоднозначно, не резолвим
    implementerOf_.clear();
    ambiguousImpl_.clear();
    for (const auto& [unitPath, ast] : unitAsts) {
        const auto* nodes = ast->getArray("nodes");
        if (!nodes) continue;
        for (const auto& n : *nodes) {
            const auto* c = n.getAsObject();
            if (!c || nodeType(*c) != "ContractDefinition" ||
                strField(*c, "contractKind") != "contract") continue;
            auto cid = c->getInteger("id");
            const auto* lin = c->getArray("linearizedBaseContracts");
            if (!cid || !lin) continue;
            for (const auto& b : *lin)
                if (auto bid = b.getAsInteger())
                    if (*bid != *cid) {
                        if (implementerOf_.count(*bid) && implementerOf_[*bid] != c)
                            ambiguousImpl_.insert(*bid);
                        implementerOf_[*bid] = c;
                    }
        }
    }

    // выбор контрактов: targetContract (режим --address) — только он, по всем файлам;
    // иначе — все contract-kind из главного файла (single-file --input)
    for (const auto& [unitPath, ast] : unitAsts) {
        const auto* nodes = ast->getArray("nodes");
        if (!nodes) continue;
        for (const auto& n : *nodes) {
            const auto* no = n.getAsObject();
            if (!no || nodeType(*no) != "ContractDefinition") continue;
            if (strField(*no, "contractKind") != "contract") continue;   // interface/library — пропуск
            if (!targetContract.empty()) {
                if (strField(*no, "name") != targetContract) continue;   // только цель
            } else if (unitPath != primaryUnit) {
                continue;   // single-file: только главный файл
            }
            lowerContract(*no, unitPath);
        }
    }
    return std::move(modules_);
}

}  // namespace solc::codegen
