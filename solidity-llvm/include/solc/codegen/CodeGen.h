#pragma once
#include <memory>
#include <string>
#include <unordered_map>

#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include "solc/ast/AST.h"
#include "solc/codegen/EvmIntrinsics.h"
#include "solc/diag/Diagnostic.h"

namespace solc::codegen {

struct LocalSlot {
    llvm::AllocaInst* alloca;
    llvm::Type*       type;
};

// storage-ссылка старого Solidity: `var acc = balances[msg.sender]` для storage-
// агрегата (mapping=>struct) НЕ копирует, а создаёт указатель в storage. поэтому
// `acc.field = x` пишет в саму ячейку. моделируем это, чтобы не терять эффект
// (иначе reentrancy-запись после external call пропадает — ложный негатив)
struct StorageAlias {
    int              baseSlot;   // слот mapping-переменной
    const ast::Expr* key;        // ключ (AST-узел, живёт всё время codegen); переэмитим
};

class CodeGen {
public:
    explicit CodeGen(DiagnosticEngine& diag);
    std::unique_ptr<llvm::Module> generate(const ast::ContractDecl& c);

private:
    llvm::Type*     llvmType(ast::TypeKind k);
    llvm::Function* emitFunctionProto(const ast::FunctionDecl& fn);
    void            emitFunctionBody(const ast::FunctionDecl& fn, llvm::Function* func);
    // инлайнинг модификаторов: тело модификатора вокруг тела функции, _; -> следующий уровень
    void            emitWithModifiers(const ast::FunctionDecl& fn, size_t modIdx);
    llvm::Value*    emitExpr(const ast::Expr& e);
    void            emitStmt(const ast::Stmt& s);
    llvm::Value*    emitStateLoad(int slot, ast::TypeKind k);
    void            emitStateStore(int slot, llvm::Value* v, ast::TypeKind k);

    // storage моделируем как указатель в addrspace(1): slot -> inttoptr -> load/store.
    // именно addrspace(1)-запись после external_call — сигнатура reentrancy (AD-3)
    llvm::Value*    storageSlotPtr(int slot);
    llvm::Value*    emitMapElemPtr(const ast::IndexExpr& ix);   // ptr addrspace(1) на элемент mapping
    llvm::Value*    emitMapElemPtrRaw(int baseSlot, const ast::Expr& key);  // то же по слоту+ключу
    void            tagStorage(llvm::Instruction* inst, int slot);
    // укоренён ли lvalue в storage (state var) — для маркера непрозрачной записи
    bool            rootsAtStorage(const ast::Expr& e) const;
    void            tagExternalCall(llvm::Instruction* call, const char* kindStr);

    // debug-info: инициализация DIBuilder/DICompileUnit и привязка DISubprogram
    void            initDebugInfo(const ast::ContractDecl& c);
    // выставить текущую отладочную позицию (line/col) в области текущей DISubprogram
    void            setDebugLoc(const SourceLoc& loc);

    // адресные пространства EVM в LLVM IR
    static constexpr unsigned kStorageAS  = 1;  // storage — постоянное состояние
    static constexpr unsigned kMemoryAS   = 2;  // memory — временное
    static constexpr unsigned kCalldataAS = 3;  // calldata — read-only вход

    DiagnosticEngine&                              diag_;
    std::unique_ptr<llvm::LLVMContext>             ctx_;
    std::unique_ptr<llvm::Module>                  module_;
    std::unique_ptr<llvm::IRBuilder<>>             builder_;
    EvmIntrinsics                                  intr_;
    std::unordered_map<std::string, LocalSlot>     locals_;
    std::unordered_map<std::string, StorageAlias>  storageAliases_;   // var-ссылки на storage
    std::unordered_map<std::string, int>           stateVarSlots_;
    std::unordered_map<std::string, ast::TypeKind> stateVarTypes_;
    std::unordered_map<std::string, const ast::ModifierDecl*> modifierMap_;
    std::unordered_map<std::string, llvm::Function*>          funcByName_;   // для внутренних вызовов
    llvm::Function*                                currentFn_ = nullptr;
    int                                            exprDepth_ = 0;   // защита стека на глубоких AST
    static constexpr int kMaxExprDepth = 400;

    // debug-info: строитель метаданных, файл и compile unit на модуль,
    // текущая DISubprogram — область для всех !dbg внутри тела функции
    std::unique_ptr<llvm::DIBuilder>               dib_;
    llvm::DIFile*                                  diFile_ = nullptr;
    llvm::DICompileUnit*                           diCU_   = nullptr;
    llvm::DISubprogram*                            diSP_   = nullptr;
};

}  // namespace solc::codegen
