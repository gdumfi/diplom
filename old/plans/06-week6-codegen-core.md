# Week 6 — Codegen Core (LLVM IR emission)

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development или superpowers:executing-plans.

**Goal:** Превратить семантически проверенный AST в валидный LLVM IR. State vars читаются/пишутся через intrinsic'и `evm.sload`/`evm.sstore`, локальные — через `alloca`+`load`/`store`, выражения через LLVM `add`/`icmp`/..., управление потоком через `br`/`CondBr`. По итогам недели `Counter.sol` БЕЗ `require`/`msg.*` компилируется в `.ll`, проходящий `llvm-as`.

**Architecture:** Класс `CodeGen` владеет `llvm::LLVMContext`, `llvm::Module`, `llvm::IRBuilder<>`. Обход AST — рекурсивные switch'и по `kind`. Сначала декларируются 2 intrinsic'а (sload, sstore), затем все функции (proto + body). Локалы и параметры хранятся в `std::unordered_map<std::string, LocalSlot>` (alloca-инст + LLVM-тип), очищается на каждой функции. State vars не имеют alloca — каждое чтение/запись разворачивается в call к intrinsic'у с константой-слотом.

**Tech Stack:** C++20, LLVM 18 C++ API (Module, IRBuilder, ConstantInt, APInt, BasicBlock, Function, verifier).

---

## File Structure

- **Create:** `include/solc/codegen/EvmIntrinsics.h` — `struct EvmIntrinsics`; функция `declareEvmIntrinsics(Module&)`.
- **Create:** `src/codegen/EvmIntrinsics.cpp` — реализация.
- **Create:** `include/solc/codegen/CodeGen.h` — `class CodeGen`, public `generate(const ContractDecl&) -> std::unique_ptr<Module>`.
- **Create:** `src/codegen/CodeGen.cpp` — основная логика.
- **Modify:** `src/CMakeLists.txt` — добавить два новых cpp + проверить что нужные LLVM компоненты слинкованы (`core support irreader bitwriter analysis` уже подключены, должно хватить).
- **Modify:** `src/driver/Driver.cpp` — на флаге `emitIR` запускать pipeline lex→parse→sema→codegen и печатать модуль (вместо stub-строки).
- **Create:** `tests/test_codegen.cpp` — ~12 TEST.
- **Modify:** `tests/CMakeLists.txt` — добавить `test_codegen.cpp`.
- **Modify:** `tests/fixtures/` — добавить `counter_no_require.sol` (упрощённый Counter без require и msg.*).

---

## EVM intrinsics (только week 6 — sload/sstore; остальные на week 7)

```cpp
namespace solc::codegen {

struct EvmIntrinsics {
    llvm::Function* sload   = nullptr;   // i256 (i256 slot)
    llvm::Function* sstore  = nullptr;   // void (i256 slot, i256 value)
};

EvmIntrinsics declareEvmIntrinsics(llvm::Module& m);

}
```

Объявления: `ExternalLinkage`, без тела, имена `"evm.sload"`, `"evm.sstore"`. Реализации этих функций где-то «снаружи» — у нас их нет, нам важно что в IR останутся валидные `declare`.

---

## Маппинг типов

```cpp
llvm::Type* llvmType(ast::TypeKind k, llvm::LLVMContext& ctx) {
    switch (k) {
        case ast::TypeKind::Uint256: return llvm::IntegerType::get(ctx, 256);
        case ast::TypeKind::Bool:    return llvm::IntegerType::get(ctx, 1);
        case ast::TypeKind::Address: return llvm::IntegerType::get(ctx, 160);
    }
    llvm_unreachable("bad TypeKind");
}
```

Для storage-операций: значения `bool`/`address` `zext`-ятся до `i256` перед `sstore`; после `sload` `trunc`-аются обратно.

---

## CodeGen API

```cpp
namespace solc::codegen {

struct LocalSlot {
    llvm::AllocaInst* alloca;
    llvm::Type*       type;     // для CreateLoad после opaque-pointers (LLVM 16+)
};

class CodeGen {
public:
    explicit CodeGen(DiagnosticEngine& diag);

    // вернёт nullptr на ошибке verifyModule
    std::unique_ptr<llvm::Module> generate(const ast::ContractDecl& c);

private:
    llvm::Type*  llvmType(ast::TypeKind k);
    llvm::Function* emitFunctionProto(const ast::FunctionDecl& fn);
    void         emitFunctionBody(const ast::FunctionDecl& fn, llvm::Function* func);

    llvm::Value* emitExpr(const ast::Expr& e);
    void         emitStmt(const ast::Stmt& s);
    llvm::Value* emitStateLoad(int slot, ast::TypeKind k);
    void         emitStateStore(int slot, llvm::Value* v, ast::TypeKind k);

    DiagnosticEngine&                       diag_;
    std::unique_ptr<llvm::LLVMContext>      ctx_;
    std::unique_ptr<llvm::Module>           module_;
    std::unique_ptr<llvm::IRBuilder<>>      builder_;
    EvmIntrinsics                           intr_;
    std::unordered_map<std::string, LocalSlot> locals_;   // сброс на каждую функцию
    llvm::Function*                         currentFn_ = nullptr;
};

}
```

---

## Tasks

### Task 1: CodeGen skeleton + intrinsic declarations

**Files:** EvmIntrinsics.h, EvmIntrinsics.cpp, CodeGen.h, CodeGen.cpp, src/CMakeLists.txt, tests/test_codegen.cpp, tests/CMakeLists.txt.

- [ ] **Step 1.1: Failing test**

Создай `tests/test_codegen.cpp`:
```cpp
#include <gtest/gtest.h>
#include <llvm/IR/Verifier.h>
#include "solc/codegen/CodeGen.h"
#include "solc/ast/AST.h"

TEST(CodeGen, EmitsEmptyModule) {
    solc::ast::ContractDecl c;
    c.name = "X";

    solc::DiagnosticEngine d;
    solc::codegen::CodeGen cg(d);
    auto mod = cg.generate(c);
    ASSERT_NE(mod, nullptr);
    EXPECT_FALSE(llvm::verifyModule(*mod, nullptr));   // false = no errors
}

TEST(CodeGen, DeclaresSloadAndSstore) {
    solc::ast::ContractDecl c;
    c.name = "X";

    solc::DiagnosticEngine d;
    solc::codegen::CodeGen cg(d);
    auto mod = cg.generate(c);
    ASSERT_NE(mod, nullptr);

    auto* sload  = mod->getFunction("evm.sload");
    auto* sstore = mod->getFunction("evm.sstore");
    ASSERT_NE(sload, nullptr);
    ASSERT_NE(sstore, nullptr);
    // signature: i256 (i256), void (i256, i256)
    EXPECT_TRUE(sload->getReturnType()->isIntegerTy(256));
    EXPECT_EQ(sload->arg_size(), 1u);
    EXPECT_TRUE(sstore->getReturnType()->isVoidTy());
    EXPECT_EQ(sstore->arg_size(), 2u);
}
```

- [ ] **Step 1.2: `include/solc/codegen/EvmIntrinsics.h`**

```cpp
#pragma once
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>

namespace solc::codegen {

struct EvmIntrinsics {
    llvm::Function* sload  = nullptr;
    llvm::Function* sstore = nullptr;
};

EvmIntrinsics declareEvmIntrinsics(llvm::Module& m);

}  // namespace solc::codegen
```

- [ ] **Step 1.3: `src/codegen/EvmIntrinsics.cpp`**

```cpp
#include "solc/codegen/EvmIntrinsics.h"
#include <llvm/IR/DerivedTypes.h>

namespace solc::codegen {

EvmIntrinsics declareEvmIntrinsics(llvm::Module& m) {
    auto& ctx = m.getContext();
    auto* i256 = llvm::IntegerType::get(ctx, 256);
    auto* voidTy = llvm::Type::getVoidTy(ctx);

    EvmIntrinsics e;

    // i256 @evm.sload(i256)
    auto* sloadTy = llvm::FunctionType::get(i256, {i256}, false);
    e.sload = llvm::Function::Create(
        sloadTy, llvm::Function::ExternalLinkage, "evm.sload", &m);

    // void @evm.sstore(i256, i256)
    auto* sstoreTy = llvm::FunctionType::get(voidTy, {i256, i256}, false);
    e.sstore = llvm::Function::Create(
        sstoreTy, llvm::Function::ExternalLinkage, "evm.sstore", &m);

    return e;
}

}  // namespace solc::codegen
```

- [ ] **Step 1.4: `include/solc/codegen/CodeGen.h`**

```cpp
#pragma once
#include <memory>
#include <string>
#include <unordered_map>

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

class CodeGen {
public:
    explicit CodeGen(DiagnosticEngine& diag);
    std::unique_ptr<llvm::Module> generate(const ast::ContractDecl& c);

private:
    llvm::Type*     llvmType(ast::TypeKind k);

    DiagnosticEngine&                          diag_;
    std::unique_ptr<llvm::LLVMContext>         ctx_;
    std::unique_ptr<llvm::Module>              module_;
    std::unique_ptr<llvm::IRBuilder<>>         builder_;
    EvmIntrinsics                              intr_;
    std::unordered_map<std::string, LocalSlot> locals_;
    llvm::Function*                            currentFn_ = nullptr;
};

}  // namespace solc::codegen
```

- [ ] **Step 1.5: `src/codegen/CodeGen.cpp` начальная версия**

```cpp
#include "solc/codegen/CodeGen.h"
#include <llvm/IR/Verifier.h>

namespace solc::codegen {

CodeGen::CodeGen(DiagnosticEngine& diag) : diag_(diag) {}

llvm::Type* CodeGen::llvmType(ast::TypeKind k) {
    switch (k) {
        case ast::TypeKind::Uint256: return llvm::IntegerType::get(*ctx_, 256);
        case ast::TypeKind::Bool:    return llvm::IntegerType::get(*ctx_, 1);
        case ast::TypeKind::Address: return llvm::IntegerType::get(*ctx_, 160);
    }
    llvm_unreachable("bad TypeKind");
}

std::unique_ptr<llvm::Module> CodeGen::generate(const ast::ContractDecl& c) {
    ctx_     = std::make_unique<llvm::LLVMContext>();
    module_  = std::make_unique<llvm::Module>(c.name, *ctx_);
    builder_ = std::make_unique<llvm::IRBuilder<>>(*ctx_);
    intr_    = declareEvmIntrinsics(*module_);

    // тело функций — следующие задачи

    if (llvm::verifyModule(*module_, nullptr)) {
        diag_.error({}, "internal: module failed verification");
        return nullptr;
    }
    return std::move(module_);
}

}  // namespace solc::codegen
```

- [ ] **Step 1.6: Wire in build**

В `src/CMakeLists.txt` добавь `codegen/EvmIntrinsics.cpp` и `codegen/CodeGen.cpp` в источники `solc_core`. Создай каталог `src/codegen/`.

В `tests/CMakeLists.txt` добавь `test_codegen.cpp`.

- [ ] **Step 1.7: Build + test**
```bash
cd /home/akum1/diplom/solidity-llvm/build
cmake ..
ninja unit_tests && ctest --output-on-failure
```
Expected: 82/82 (80 + 2 новых).

- [ ] **Step 1.8: Commit**
```bash
cd /home/akum1/diplom/solidity-llvm
git add include/solc/codegen src/codegen src/CMakeLists.txt tests/CMakeLists.txt tests/test_codegen.cpp
git commit -m "feat(codegen): CodeGen skeleton with EVM intrinsics declarations"
```

---

### Task 2: Function prototypes + entry block + параметры

**Files:** CodeGen.h, CodeGen.cpp, test_codegen.cpp.

- [ ] **Step 2.1: Failing test**

Добавь в `tests/test_codegen.cpp`:
```cpp
#include "solc/lexer/Lexer.h"
#include "solc/parser/Parser.h"
#include "solc/sema/Sema.h"

static std::unique_ptr<llvm::Module> compileSrc(const char* src, solc::DiagnosticEngine& d) {
    solc::Lexer lex(src, d);
    solc::Parser p(lex, d);
    auto c = p.parseContract();
    if (!c) return nullptr;
    solc::sema::SemanticAnalyzer sa(d);
    if (!sa.analyze(*c)) return nullptr;
    solc::codegen::CodeGen cg(d);
    return cg.generate(*c);
}

TEST(CodeGen, FunctionPrototypesEmitted) {
    solc::DiagnosticEngine d;
    auto mod = compileSrc(
        "contract C {"
        "  function f(uint256 x) public returns (uint256) { return x; }"
        "}", d);
    ASSERT_NE(mod, nullptr);
    auto* f = mod->getFunction("f");
    ASSERT_NE(f, nullptr);
    EXPECT_TRUE(f->getReturnType()->isIntegerTy(256));
    EXPECT_EQ(f->arg_size(), 1u);
    EXPECT_TRUE(f->getArg(0)->getType()->isIntegerTy(256));
}

TEST(CodeGen, ParameterAllocaInEntry) {
    solc::DiagnosticEngine d;
    auto mod = compileSrc(
        "contract C {"
        "  function f(uint256 x) public returns (uint256) { return x; }"
        "}", d);
    ASSERT_NE(mod, nullptr);
    auto* f = mod->getFunction("f");
    ASSERT_NE(f, nullptr);
    ASSERT_FALSE(f->empty());
    auto& entry = f->getEntryBlock();
    int allocaCount = 0;
    for (auto& inst : entry) {
        if (llvm::isa<llvm::AllocaInst>(&inst)) allocaCount++;
    }
    EXPECT_EQ(allocaCount, 1);   // один param → один alloca
}
```

- [ ] **Step 2.2: Расширь `CodeGen.h`**

В приватной секции добавь:
```cpp
    llvm::Function* emitFunctionProto(const ast::FunctionDecl& fn);
    void            emitFunctionBody(const ast::FunctionDecl& fn, llvm::Function* func);
    llvm::Value*    emitExpr(const ast::Expr& e);
    void            emitStmt(const ast::Stmt& s);
```

- [ ] **Step 2.3: Реализация в `CodeGen.cpp`**

```cpp
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

void CodeGen::emitFunctionBody(const ast::FunctionDecl& fn, llvm::Function* func) {
    locals_.clear();
    currentFn_ = func;

    auto* entry = llvm::BasicBlock::Create(*ctx_, "entry", func);
    builder_->SetInsertPoint(entry);

    // alloca для каждого параметра + store входящего значения
    auto argIt = func->arg_begin();
    for (const auto& p : fn.params) {
        auto* ty   = llvmType(p.type.kind);
        auto* alc  = builder_->CreateAlloca(ty, nullptr, p.name);
        builder_->CreateStore(&*argIt, alc);
        locals_[p.name] = LocalSlot{alc, ty};
        ++argIt;
    }

    // обход body — следующие задачи; пока пусто => функции без терминатора будут падать verify.
    // временно: если return-тип i256/i1/i160 — вернуть нулевую константу.
    if (!fn.body.stmts.empty()) {
        for (const auto& sp : fn.body.stmts) emitStmt(*sp);
    }
    // safety net на пустое тело (Task 9 уберёт)
    if (!builder_->GetInsertBlock()->getTerminator()) {
        if (func->getReturnType()->isVoidTy()) {
            builder_->CreateRetVoid();
        } else {
            builder_->CreateRet(llvm::Constant::getNullValue(func->getReturnType()));
        }
    }
    currentFn_ = nullptr;
}

// заглушки, заполняем в Tasks 3-8
llvm::Value* CodeGen::emitExpr(const ast::Expr&) { return nullptr; }
void         CodeGen::emitStmt(const ast::Stmt&) {}
```

В `generate` добавь между `intr_ = ...` и `verifyModule(...)`:
```cpp
    for (const auto& fn : c.functions) {
        auto* func = emitFunctionProto(fn);
        emitFunctionBody(fn, func);
    }
```

- [ ] **Step 2.4: Build + test**
```bash
ninja unit_tests && ctest --output-on-failure
```
Expected: 84/84.

- [ ] **Step 2.5: Commit**
```bash
git commit -am "feat(codegen): function prototypes + entry block + parameter allocas"
```

---

### Task 3: Литералы и IdentExpr (params/locals)

**Files:** CodeGen.cpp, test_codegen.cpp.

- [ ] **Step 3.1: Failing test**
```cpp
TEST(CodeGen, ReturnsParamThroughLoad) {
    solc::DiagnosticEngine d;
    auto mod = compileSrc(
        "contract C {"
        "  function f(uint256 x) public returns (uint256) { return x; }"
        "}", d);
    ASSERT_NE(mod, nullptr);
    auto* f = mod->getFunction("f");
    ASSERT_NE(f, nullptr);

    // ищем хотя бы один load из alloca + ret
    int loads = 0, rets = 0;
    for (auto& bb : *f) for (auto& inst : bb) {
        if (llvm::isa<llvm::LoadInst>(&inst))   loads++;
        if (llvm::isa<llvm::ReturnInst>(&inst)) rets++;
    }
    EXPECT_GE(loads, 1);
    EXPECT_EQ(rets, 1);
}
```

- [ ] **Step 3.2: Реализуй `emitExpr` для Literals и IdentExpr (param/local)**

Замени заглушку:
```cpp
llvm::Value* CodeGen::emitExpr(const ast::Expr& e) {
    using namespace ast;
    switch (e.kind) {
        case ExprKind::NumberLit: {
            const auto& n = static_cast<const NumberLit&>(e);
            return llvm::ConstantInt::get(*ctx_, llvm::APInt(256, n.text, 10));
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
            // StateVar — Task 4
            diag_.error(id.loc, "ident codegen not implemented for this kind yet");
            return nullptr;
        }
        default:
            diag_.error(e.loc, "expr codegen not implemented yet");
            return nullptr;
    }
}
```

И добавь `emitStmt` минимум для `Return`:
```cpp
void CodeGen::emitStmt(const ast::Stmt& s) {
    using namespace ast;
    switch (s.kind) {
        case StmtKind::Return: {
            const auto& r = static_cast<const ReturnStmt&>(s);
            if (r.value) {
                auto* v = emitExpr(*r.value);
                if (!v) return;
                builder_->CreateRet(v);
            } else {
                builder_->CreateRetVoid();
            }
            return;
        }
        default:
            // остальные кейсы — следующие задачи
            return;
    }
}
```

- [ ] **Step 3.3: Build + test**
```bash
ninja unit_tests && ctest --output-on-failure
```
Expected: 85/85.

- [ ] **Step 3.4: Commit**
```bash
git commit -am "feat(codegen): literals, identifier loads, return statement"
```

---

### Task 4: State vars через evm.sload/sstore

**Files:** CodeGen.h, CodeGen.cpp, test_codegen.cpp.

- [ ] **Step 4.1: Failing tests**
```cpp
TEST(CodeGen, StateVarReadEmitsSload) {
    solc::DiagnosticEngine d;
    auto mod = compileSrc(
        "contract C {"
        "  uint256 value;"
        "  function f() public returns (uint256) { return value; }"
        "}", d);
    ASSERT_NE(mod, nullptr);
    auto* f = mod->getFunction("f");
    ASSERT_NE(f, nullptr);
    int sloadCalls = 0;
    for (auto& bb : *f) for (auto& inst : bb) {
        if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
            if (call->getCalledFunction()
                && call->getCalledFunction()->getName() == "evm.sload") {
                sloadCalls++;
            }
        }
    }
    EXPECT_EQ(sloadCalls, 1);
}

TEST(CodeGen, StateVarWriteEmitsSstore) {
    solc::DiagnosticEngine d;
    auto mod = compileSrc(
        "contract C {"
        "  uint256 value;"
        "  function f(uint256 x) public { value = x; }"
        "}", d);
    ASSERT_NE(mod, nullptr);
    auto* f = mod->getFunction("f");
    ASSERT_NE(f, nullptr);
    int sstoreCalls = 0;
    for (auto& bb : *f) for (auto& inst : bb) {
        if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
            if (call->getCalledFunction()
                && call->getCalledFunction()->getName() == "evm.sstore") {
                sstoreCalls++;
            }
        }
    }
    EXPECT_EQ(sstoreCalls, 1);
}
```

- [ ] **Step 4.2: Реализация state-var helpers + расширение emitExpr/emitStmt**

В `CodeGen.h` приватные:
```cpp
    llvm::Value* emitStateLoad(int slot, ast::TypeKind k);
    void         emitStateStore(int slot, llvm::Value* v, ast::TypeKind k);
```

В `CodeGen.cpp`:
```cpp
llvm::Value* CodeGen::emitStateLoad(int slot, ast::TypeKind k) {
    auto* i256 = llvm::IntegerType::get(*ctx_, 256);
    auto* slotConst = llvm::ConstantInt::get(i256, slot);
    auto* raw = builder_->CreateCall(intr_.sload, {slotConst}, "sload");
    auto* targetTy = llvmType(k);
    if (targetTy->isIntegerTy(256)) return raw;
    // bool/address — trunc до нужной ширины
    return builder_->CreateTrunc(raw, targetTy, "trunc");
}

void CodeGen::emitStateStore(int slot, llvm::Value* v, ast::TypeKind k) {
    auto* i256 = llvm::IntegerType::get(*ctx_, 256);
    auto* slotConst = llvm::ConstantInt::get(i256, slot);
    llvm::Value* wide = v;
    if (!v->getType()->isIntegerTy(256)) {
        wide = builder_->CreateZExt(v, i256, "zext");
    }
    builder_->CreateCall(intr_.sstore, {slotConst, wide});
}
```

В `emitExpr` Ident case — добавь ветку StateVar:
```cpp
            if (id.resolvedKind == IdentExpr::Resolution::StateVar) {
                return emitStateLoad(id.resolvedSlot, id.resolvedType->kind);
            }
```
(До текущей проверки Param/Local, либо после — порядок не важен.)

В `emitStmt` добавь Assign:
```cpp
        case StmtKind::Assign: {
            const auto& a = static_cast<const AssignStmt&>(s);
            auto* val = emitExpr(*a.value);
            if (!val) return;
            // target — либо state var, либо local/param. Sema гарантировала, что он есть.
            // решаем через lookup в locals_ и через подсказку — нужно знать kind/slot.
            // самый простой путь: если target в locals_ — local/param, иначе ищем как state var.
            auto it = locals_.find(a.target);
            if (it != locals_.end()) {
                builder_->CreateStore(val, it->second.alloca);
            } else {
                // state var — найдём его storageSlot и kind через текущий contract
                // ВНИМАНИЕ: AssignStmt не хранит storageSlot. Нужно держать map (name → slot/kind) при заходе в codegen
                diag_.error(a.loc, "internal: state var assign needs slot info");
            }
            return;
        }
```

**Тонкость:** `AssignStmt::target` — это просто строка-имя, без аннотации kind/slot. Sema не записала туда `Resolution`. Чтобы это работало, держи в CodeGen вспомогательный `std::unordered_map<std::string, int> stateVarSlots_` + `std::unordered_map<std::string, ast::TypeKind> stateVarTypes_`, заполняй их при входе в `generate` (обход `c.stateVars`).

Добавь в `CodeGen.h` приватно:
```cpp
    std::unordered_map<std::string, int>             stateVarSlots_;
    std::unordered_map<std::string, ast::TypeKind>   stateVarTypes_;
```

В `generate` перед обходом функций добавь:
```cpp
    for (const auto& sv : c.stateVars) {
        stateVarSlots_[sv.name] = sv.storageSlot;
        stateVarTypes_[sv.name] = sv.type.kind;
    }
```

Тогда в Assign:
```cpp
            if (it != locals_.end()) {
                builder_->CreateStore(val, it->second.alloca);
            } else {
                auto sit = stateVarSlots_.find(a.target);
                if (sit == stateVarSlots_.end()) {
                    diag_.error(a.loc, "internal: unknown assign target");
                    return;
                }
                emitStateStore(sit->second, val, stateVarTypes_[a.target]);
            }
```

- [ ] **Step 4.3: Build + test**
```bash
ninja unit_tests && ctest --output-on-failure
```
Expected: 87/87. Все предыдущие тесты по-прежнему зелёные.

- [ ] **Step 4.4: Commit**
```bash
git commit -am "feat(codegen): state variable read/write via evm.sload/sstore"
```

---

### Task 5: Binary и Unary операторы

**Files:** CodeGen.cpp, test_codegen.cpp.

- [ ] **Step 5.1: Failing tests**
```cpp
TEST(CodeGen, AddProducesIRAdd) {
    solc::DiagnosticEngine d;
    auto mod = compileSrc(
        "contract C {"
        "  function f(uint256 a, uint256 b) public returns (uint256) { return a + b; }"
        "}", d);
    ASSERT_NE(mod, nullptr);
    auto* f = mod->getFunction("f");
    int adds = 0;
    for (auto& bb : *f) for (auto& inst : bb) {
        if (inst.getOpcode() == llvm::Instruction::Add) adds++;
    }
    EXPECT_GE(adds, 1);
}

TEST(CodeGen, EqEmitsICmpEQ) {
    solc::DiagnosticEngine d;
    auto mod = compileSrc(
        "contract C {"
        "  function f(uint256 a, uint256 b) public returns (bool) { return a == b; }"
        "}", d);
    ASSERT_NE(mod, nullptr);
    auto* f = mod->getFunction("f");
    int eqs = 0;
    for (auto& bb : *f) for (auto& inst : bb) {
        if (auto* icmp = llvm::dyn_cast<llvm::ICmpInst>(&inst))
            if (icmp->getPredicate() == llvm::ICmpInst::ICMP_EQ) eqs++;
    }
    EXPECT_EQ(eqs, 1);
}

TEST(CodeGen, NotEmitsXorWithOne) {
    solc::DiagnosticEngine d;
    auto mod = compileSrc(
        "contract C {"
        "  function f(bool b) public returns (bool) { return !b; }"
        "}", d);
    ASSERT_NE(mod, nullptr);
    auto* f = mod->getFunction("f");
    int xors = 0;
    for (auto& bb : *f) for (auto& inst : bb) {
        if (inst.getOpcode() == llvm::Instruction::Xor) xors++;
    }
    EXPECT_GE(xors, 1);
}
```

- [ ] **Step 5.2: Расширь `emitExpr` Binary и Unary**

В switch добавь:
```cpp
        case ExprKind::Binary: {
            const auto& bin = static_cast<const BinaryExpr&>(e);
            auto* L = emitExpr(*bin.lhs);
            auto* R = emitExpr(*bin.rhs);
            if (!L || !R) return nullptr;
            switch (bin.op) {
                case BinOp::Add:    return builder_->CreateAdd(L, R, "add");
                case BinOp::Sub:    return builder_->CreateSub(L, R, "sub");
                case BinOp::Mul:    return builder_->CreateMul(L, R, "mul");
                case BinOp::Div:    return builder_->CreateUDiv(L, R, "udiv");
                case BinOp::Eq:     return builder_->CreateICmpEQ(L, R, "eq");
                case BinOp::NotEq:  return builder_->CreateICmpNE(L, R, "ne");
                case BinOp::Lt:     return builder_->CreateICmpULT(L, R, "lt");
                case BinOp::Gt:     return builder_->CreateICmpUGT(L, R, "gt");
                case BinOp::LtEq:   return builder_->CreateICmpULE(L, R, "le");
                case BinOp::GtEq:   return builder_->CreateICmpUGE(L, R, "ge");
                case BinOp::AndAnd: return builder_->CreateAnd(L, R, "and");
                case BinOp::OrOr:   return builder_->CreateOr(L, R, "or");
            }
            return nullptr;
        }
        case ExprKind::Unary: {
            const auto& un = static_cast<const UnaryExpr&>(e);
            auto* V = emitExpr(*un.operand);
            if (!V) return nullptr;
            if (un.op == UnOp::Not) {
                auto* one = llvm::ConstantInt::get(V->getType(), 1);
                return builder_->CreateXor(V, one, "not");
            }
            return nullptr;
        }
```

**Заметка:** `&&`/`||` через `And`/`Or` без short-circuit семантики — это упрощение. В Solidity-MVP побочных эффектов в правой части обычно нет (операнды чисто читают значения), и `Sema` уже гарантировал, что оба — `bool`. Если позже понадобится short-circuit — переделать через blocks+phi.

- [ ] **Step 5.3: Build + test**
```bash
ninja unit_tests && ctest --output-on-failure
```
Expected: 90/90.

- [ ] **Step 5.4: Commit**
```bash
git commit -am "feat(codegen): binary and unary operators"
```

---

### Task 6: VarDecl и явный Assign к local

**Files:** CodeGen.cpp, test_codegen.cpp.

VarDecl создаёт `alloca` + сохраняет init. Assign к local'у через локальный map. Это уже частично сделано — нужно убедиться + тест.

- [ ] **Step 6.1: Failing test**
```cpp
TEST(CodeGen, VarDeclCreatesAllocaAndStore) {
    solc::DiagnosticEngine d;
    auto mod = compileSrc(
        "contract C {"
        "  function f(uint256 a, uint256 b) public returns (uint256) {"
        "    uint256 y = a + b;"
        "    return y;"
        "  }"
        "}", d);
    ASSERT_NE(mod, nullptr);
    auto* f = mod->getFunction("f");
    int allocas = 0, stores = 0;
    for (auto& bb : *f) for (auto& inst : bb) {
        if (llvm::isa<llvm::AllocaInst>(&inst)) allocas++;
        if (llvm::isa<llvm::StoreInst>(&inst))  stores++;
    }
    EXPECT_GE(allocas, 3);   // 2 параметра + 1 local
    EXPECT_GE(stores, 3);    // 2 store параметров + 1 store init
}
```

- [ ] **Step 6.2: В `emitStmt` добавь VarDecl**

```cpp
        case StmtKind::VarDecl: {
            const auto& v = static_cast<const VarDeclStmt&>(s);
            auto* val = emitExpr(*v.init);
            if (!val) return;
            auto* ty  = llvmType(v.type.kind);
            auto* alc = builder_->CreateAlloca(ty, nullptr, v.name);
            builder_->CreateStore(val, alc);
            locals_[v.name] = LocalSlot{alc, ty};
            return;
        }
```

- [ ] **Step 6.3: Build + test**
```bash
ninja unit_tests && ctest --output-on-failure
```
Expected: 91/91.

- [ ] **Step 6.4: Commit**
```bash
git commit -am "feat(codegen): variable declaration statement"
```

---

### Task 7: if / else

**Files:** CodeGen.cpp, test_codegen.cpp.

- [ ] **Step 7.1: Failing test**
```cpp
TEST(CodeGen, IfElseEmitsBranchAndBlocks) {
    solc::DiagnosticEngine d;
    auto mod = compileSrc(
        "contract C {"
        "  function f(uint256 x) public returns (uint256) {"
        "    if (x == 0) { return 1; } else { return 2; }"
        "  }"
        "}", d);
    ASSERT_NE(mod, nullptr);
    auto* f = mod->getFunction("f");
    int branches = 0, blocks = 0;
    for (auto& bb : *f) {
        blocks++;
        for (auto& inst : bb) {
            if (llvm::isa<llvm::BranchInst>(&inst)) branches++;
        }
    }
    EXPECT_GE(blocks, 3);     // entry, then, else (merge возможно отсутствует — оба пути возвращают)
    EXPECT_GE(branches, 1);   // как минимум условный branch
}
```

- [ ] **Step 7.2: Дополнительная защита в `emitStmt` — guard на терминированный блок**

В САМОМ НАЧАЛЕ `emitStmt(const ast::Stmt& s)`, до switch:
```cpp
    // если текущий блок уже терминирован (например, после if-else, где оба
    // пути завершились return), любые следующие stmts мертвы — молча пропускаем.
    if (builder_->GetInsertBlock()->getTerminator()) return;
```

Это нужно чтобы безопасно обрабатывать «мертвый код после if/else, где оба ветки делают return».

- [ ] **Step 7.3: В `emitStmt` добавь If**

```cpp
        case StmtKind::If: {
            const auto& i = static_cast<const IfStmt&>(s);
            auto* cond = emitExpr(*i.cond);
            if (!cond) return;

            auto* thenBB  = llvm::BasicBlock::Create(*ctx_, "if.then",  currentFn_);
            auto* elseBB  = llvm::BasicBlock::Create(*ctx_, "if.else",  currentFn_);
            auto* mergeBB = llvm::BasicBlock::Create(*ctx_, "if.merge", currentFn_);

            builder_->CreateCondBr(cond, thenBB, elseBB);

            builder_->SetInsertPoint(thenBB);
            for (auto& sp : i.thenBlock) emitStmt(*sp);
            if (!builder_->GetInsertBlock()->getTerminator())
                builder_->CreateBr(mergeBB);

            builder_->SetInsertPoint(elseBB);
            for (auto& sp : i.elseBlock) emitStmt(*sp);
            if (!builder_->GetInsertBlock()->getTerminator())
                builder_->CreateBr(mergeBB);

            // если ни одна ветка не дошла до mergeBB — он мёртв.
            // удаляем его И оставляем insert point в текущем (последнем) блоке —
            // последующие emitStmt отшлёпает guard на терминированный блок.
            if (mergeBB->hasNPredecessors(0)) {
                mergeBB->eraseFromParent();
                // builder_ всё ещё указывает на elseBB (уже терминированный) —
                // следующий emitStmt вызовет ранний return из guard.
            } else {
                builder_->SetInsertPoint(mergeBB);
            }
            return;
        }
```

- [ ] **Step 7.4: Build + test**
```bash
ninja unit_tests && ctest --output-on-failure
```
Expected: 92/92.

- [ ] **Step 7.5: Commit**
```bash
git commit -am "feat(codegen): if/else control flow with merge block"
```

---

### Task 8: Driver integration + Counter без require

**Files:** Driver.cpp, tests/fixtures/counter_no_require.sol, test_codegen.cpp.

- [ ] **Step 8.1: Создай фикстуру `tests/fixtures/counter_no_require.sol`**

```solidity
contract Counter {
    uint256 public value;

    function increment(uint256 delta) public returns (uint256) {
        uint256 newValue = value + delta;
        value = newValue;
        return newValue;
    }

    function isPositive(uint256 x) public returns (bool) {
        return x > 0;
    }
}
```

(Без `owner`/`require`/`msg.sender` — это неделя 7.)

- [ ] **Step 8.2: Failing test**
```cpp
#include <fstream>
#include <sstream>
#include <llvm/Support/raw_ostream.h>

TEST(CodeGen, CounterNoRequireVerifies) {
    std::ifstream f(std::string(SOLC_TESTS_DIR) + "/fixtures/counter_no_require.sol");
    std::stringstream ss; ss << f.rdbuf();
    std::string src = ss.str();
    ASSERT_FALSE(src.empty());

    solc::DiagnosticEngine d;
    auto mod = compileSrc(src.c_str(), d);
    ASSERT_NE(mod, nullptr);
    EXPECT_FALSE(d.hasErrors());
    EXPECT_FALSE(llvm::verifyModule(*mod, nullptr));

    EXPECT_NE(mod->getFunction("increment"), nullptr);
    EXPECT_NE(mod->getFunction("isPositive"), nullptr);
    EXPECT_NE(mod->getFunction("evm.sload"), nullptr);
    EXPECT_NE(mod->getFunction("evm.sstore"), nullptr);
}
```

- [ ] **Step 8.3: Build + test**
```bash
ninja unit_tests && ctest --output-on-failure
```
Expected: 93/93.

- [ ] **Step 8.4: Интегрируй CodeGen в `Driver`**

В `src/driver/Driver.cpp` ЗАМЕНИ stub-эмиссию на полный pipeline. Найди блок:
```cpp
    if (opts.emitIR) {
        std::ostream* out = &std::cout;
        std::ofstream file;
        if (!opts.outputPath.empty()) {
            file.open(opts.outputPath);
            if (!file.is_open()) {
                std::cerr << "error: cannot open output '" << opts.outputPath << "'\n";
                return 1;
            }
            out = &file;
        }
        (*out) << "; solidity-llvm: stub output, source length="
               << source.size() << "\n";
    }
```

Замени на:
```cpp
    if (opts.emitIR) {
        solc::DiagnosticEngine diag;
        solc::Lexer lex(source, diag);
        solc::Parser p(lex, diag);
        auto contract = p.parseContract();
        if (!contract || diag.hasErrors()) {
            for (const auto& e : diag.errors()) {
                std::cerr << "error: line " << e.loc.line << ":" << e.loc.col
                          << ": " << e.message << "\n";
            }
            return 1;
        }

        solc::sema::SemanticAnalyzer sa(diag);
        if (!sa.analyze(*contract)) {
            for (const auto& e : diag.errors()) {
                std::cerr << "error: line " << e.loc.line << ":" << e.loc.col
                          << ": " << e.message << "\n";
            }
            return 1;
        }

        solc::codegen::CodeGen cg(diag);
        auto module = cg.generate(*contract);
        if (!module || diag.hasErrors()) {
            for (const auto& e : diag.errors()) {
                std::cerr << "error: line " << e.loc.line << ":" << e.loc.col
                          << ": " << e.message << "\n";
            }
            return 1;
        }

        std::string str;
        llvm::raw_string_ostream rso(str);
        module->print(rso, nullptr);

        std::ostream* out = &std::cout;
        std::ofstream file;
        if (!opts.outputPath.empty()) {
            file.open(opts.outputPath);
            if (!file.is_open()) {
                std::cerr << "error: cannot open output '" << opts.outputPath << "'\n";
                return 1;
            }
            out = &file;
        }
        (*out) << str;
    }
```

Добавь нужные includes в `Driver.cpp`:
```cpp
#include "solc/lexer/Lexer.h"
#include "solc/parser/Parser.h"
#include "solc/sema/Sema.h"
#include "solc/codegen/CodeGen.h"
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
```

- [ ] **Step 8.5: Build + проверка вручную**
```bash
ninja solidity-llvm
cd ..
./build/src/solidity-llvm --input tests/fixtures/counter_no_require.sol --emit-ir
```
Expected: настоящий LLVM IR на stdout, содержащий `define i256 @increment(...)` и `call i256 @evm.sload(...)`.

Дополнительно:
```bash
./build/src/solidity-llvm --input tests/fixtures/counter_no_require.sol --emit-ir --output /tmp/counter.ll
llvm-as /tmp/counter.ll -o /tmp/counter.bc
echo "exit: $?"
```
Expected: exit 0 — IR валидный для LLVM.

- [ ] **Step 8.6: Smoke-тесты Driver не должны сломаться**

Существующие `Smoke.DriverAcceptsExistingFile` пишет `contract X {}` (пустой контракт). Это должно по-прежнему работать: парсер примет, sema OK, codegen эмитит модуль с двумя intrinsic-объявлениями и без функций. Verify OK. Driver вернёт 0.

Проверь:
```bash
ctest --output-on-failure
```
Expected: 93/93.

- [ ] **Step 8.7: Commit**
```bash
git add src/driver/Driver.cpp tests/fixtures/counter_no_require.sol tests/test_codegen.cpp
git commit -m "feat(driver): full pipeline lex→parse→sema→codegen on --emit-ir"
```

---

## Scope boundaries

- НЕ реализовывать `require`/`msg.sender`/`msg.value` — это неделя 7.
- НЕ интегрировать `llvm-as` в тесты как `add_test` — это неделя 7 или 8.
- НЕ менять lexer/parser/sema.
- Если `Counter.sol` (с `require`) приходит на codegen — sema или codegen выдаст ошибку, и Driver вернёт != 0. Это ожидаемо. Тестовая фикстура `counter_no_require.sol` — упрощённая.

---

## Self-review checklist

- [ ] Все 4 типа выражений (литералы / ident / binary / unary) корректно эмитят IR
- [ ] State vars читаются `sload` с правильным slot
- [ ] State vars пишутся `sstore` с правильным slot + zext если bool/address
- [ ] Local vars через `alloca` + `load`/`store`
- [ ] Параметры тоже через `alloca`+`store` в entry
- [ ] `if/else` создаёт 3 блока + CondBr, корректно терминирует
- [ ] `return` через `CreateRet`
- [ ] `verifyModule` проходит на все позитивные тесты
- [ ] Counter (без require) → IR проходит `llvm-as` exit 0
- [ ] Driver на `--emit-ir` выдаёт настоящий IR, а не stub

---

## Критерий готовности недели 6

- [ ] ≥ 12 codegen-тестов зелёные
- [ ] `counter_no_require.sol` end-to-end через `./solidity-llvm --emit-ir` → `.ll` проходит `llvm-as`
- [ ] IR содержит вызовы `@evm.sload` и `@evm.sstore`
- [ ] Все if-ветви корректно терминируются (`br` или `ret`)
- [ ] `verifyModule` зелёный на всех фикстурах
- [ ] Минимум 8 коммитов (по одному на task)
