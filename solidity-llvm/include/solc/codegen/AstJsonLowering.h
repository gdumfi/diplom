#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/JSON.h>

#include "solc/codegen/EvmIntrinsics.h"
#include "solc/diag/Diagnostic.h"

namespace solc::codegen {

// понижение РАЗРЕШЁННОГО solc-AST (standard-json) в тот же EVM-размеченный LLVM IR,
// что и рукописный CodeGen. заменяет lexer+parser+sema+Inheritance: имена, типы, слоты
// и линеаризацию наследования даёт solc. downstream (детекторы/KLEE/PoC) не меняется —
// эмитим ровно IR-контракт из docs/IR-CONTRACT.md.
class AstJsonLowering {
public:
    explicit AstJsonLowering(DiagnosticEngine& diag);

    // по модулю на deployable-контракт из standard-json solc (одно- и многофайловый).
    // root — полный вывод solc; sources — все файлы (путь→текст, для line-таблиц по fileIndex);
    // primaryUnit — главный файл (single-file: его контракты); targetContract — если задан
    // (режим --address), лоуэрим ТОЛЬКО контракт с этим именем (базы OZ резолвим по всем файлам)
    std::vector<std::unique_ptr<llvm::Module>> lowerAll(
        const llvm::json::Value& root,
        const std::string& primaryUnit,
        const std::vector<std::pair<std::string, std::string>>& sources,
        const std::string& targetContract = "");

private:
    using JObj = llvm::json::Object;
    using JVal = llvm::json::Value;

    // локаль/параметр: alloca + тип (единая модель доступа через load/store)
    struct LocalSlot { llvm::AllocaInst* alloca; llvm::Type* type; };
    // storage-ссылка (`T storage x = map[k]`): x.field / bare x пишет в саму ячейку
    struct StorageAliasJ { int baseSlot; const JObj* key; };

    // --- обход одного контракта (unit — файл, где он определён, для storageLayout) ---
    void lowerContract(const JObj& contract, const std::string& unit);
    void collectInherited(const JObj& contract,
                          std::vector<const JObj*>& funcs,
                          std::vector<const JObj*>& mods);
    llvm::Function* emitFunctionProto(const JObj& fn);
    void emitFunctionBody(const JObj& fn, llvm::Function* func);
    void emitWithModifiers(const JObj& fn, size_t modIdx);
    // разрешённые ModifierDefinition текущей функции (в порядке применения), для инлайна
    std::vector<const JObj*> collectFnModifiers(const JObj& fn);

    llvm::Value* emitExpr(const JObj& e);
    void         emitStmt(const JObj& s);
    // ячейка storage для поля struct (X.field): реальный слот для plain-struct-var,
    // синтетический map_elem для mapping-of-struct. nullopt — поле не резолвится (opaque)
    struct FieldCell { llvm::Value* ptr; int slot; };
    std::optional<FieldCell> structFieldCell(const JObj& memberAccess);
    int          synthSlotFor(int baseSlot, int memberOffset);   // синтетический слот поля
    // резолв obj.method() к уникальному имплементеру интерфейса и проверка, что тот
    // метод РЕАЛЬНО пишет состояние (для подтверждённого cross-contract эффекта, 2b)
    bool         calleeWritesState(const JObj& calleeMemberAccess);
    void         emitBlockLike(const JVal* node);          // Block | одиночный statement | null
    void         emitRequire(const JObj& condExpr);        // require/assert -> branch+revert
    void         storeToLValue(const JObj& lhs, llvm::Value* val);
    llvm::Value* emitAssignment(const JObj& a);
    llvm::Value* emitFunctionCall(const JObj& c);

    // --- storage / интринсики (форма IR-контракта) ---
    llvm::Value* emitStateLoad(int slot, llvm::StringRef typeStr);
    void         emitStateStore(int slot, llvm::Value* v);
    llvm::Value* storageSlotPtr(int slot);
    llvm::Value* emitMapElemPtrRaw(int baseSlot, const JObj& keyExpr);
    void         tagStorage(llvm::Instruction* inst, int slot);
    void         tagExternalCall(llvm::Instruction* call, const char* kindStr);
    bool         rootsAtStorage(const JObj& e) const;

    // --- типы ---
    llvm::Type* typeFromStr(llvm::StringRef ts);

    // --- разрешение по solc-AST ---
    const JObj* nodeById(int64_t id) const;               // узел по id (idIndex_)
    static llvm::StringRef nodeType(const JObj& n);
    static llvm::StringRef strField(const JObj& n, llvm::StringRef k);
    int  slotOfDecl(int64_t declId) const;                // astId стейт-вара -> слот, иначе -1

    // --- debug-info ---
    void initDebugInfo(const std::string& contractName);
    void setDebugLocFromSrc(const JObj& n);
    static std::vector<size_t> buildLineStarts(const std::string& source);
    unsigned lineOfSrc(llvm::StringRef src) const;   // src = "start:len:fileIndex"

    static constexpr unsigned kStorageAS = 1;
    static constexpr int kMaxExprDepth = 400;

    DiagnosticEngine& diag_;
    const JObj* root_ = nullptr;
    std::string unit_;                                    // текущий файл (для storageLayout)
    std::unordered_map<int64_t, const JObj*> idIndex_;    // id -> узел по ВСЕМ файлам
    std::unordered_map<int, std::vector<size_t>> fileLineTables_;  // fileIndex -> начала строк

    // каждый модуль ссылается на свой контекст; контексты обязаны пережить модули,
    // поэтому владельцев держим здесь (ctx_ — невладеющий указатель на текущий)
    std::vector<std::unique_ptr<llvm::LLVMContext>> contexts_;
    llvm::LLVMContext* ctx_ = nullptr;
    std::unique_ptr<llvm::Module>      module_;
    std::unique_ptr<llvm::IRBuilder<>> builder_;
    EvmIntrinsics intr_;

    std::unordered_map<std::string, LocalSlot>     locals_;
    std::unordered_map<std::string, StorageAliasJ> storageAliases_;
    std::unordered_map<int64_t, int>               slotById_;      // astId стейт-вара -> слот
    std::unordered_map<int64_t, std::string>       typeStrById_;   // astId стейт-вара -> typeString
    std::unordered_map<int64_t, int>               memberSlotById_; // astId члена struct -> offset в struct
    std::map<std::pair<int,int>, int>              synthSlot_;     // (базовый слот, offset) -> синтет. слот
    int                                            synthNext_ = 0; // счётчик синтет. слотов (над реальными)
    std::unordered_map<int64_t, const JObj*>       implementerOf_; // id интерфейса -> контракт-имплементер
    std::unordered_set<int64_t>                    ambiguousImpl_; // интерфейсы с >1 имплементером (не резолвим)
    std::unordered_map<std::string, const JObj*>   modifierByName_;
    std::unordered_map<std::string, llvm::Function*> funcByName_;
    std::vector<const JObj*> curMods_;   // модификаторы текущей функции (для emitWithModifiers)
    llvm::Function* currentFn_ = nullptr;
    int exprDepth_ = 0;

    std::unique_ptr<llvm::DIBuilder> dib_;
    llvm::DIFile*        diFile_ = nullptr;
    llvm::DICompileUnit* diCU_   = nullptr;
    llvm::DISubprogram*  diSP_    = nullptr;
    std::vector<std::unique_ptr<llvm::Module>> modules_;   // результат
};

}  // namespace solc::codegen
