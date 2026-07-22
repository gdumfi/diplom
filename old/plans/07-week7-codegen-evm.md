# Week 7 — Codegen: require, msg.*, EVM intrinsics

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans.

**Goal:** Допилить EVM-специфичную часть codegen: `require(cond)`, `msg.sender` (i160), `msg.value` (i256), декларация и использование всех EVM intrinsic-функций. После этой недели полный `Counter.sol` компилируется в `.ll`, проходящий `llvm-as`.

**Architecture:** Добавляем intrinsics: `evm.caller`, `evm.callvalue`, `evm.revert`. `require` разворачивается в conditional branch: при истине — продолжаем, при лжи — переход на revert-блок, в котором вызывается `evm.revert(0, 0)` и `unreachable`. `msg.sender` — прямой вызов `evm.caller`.

**Tech Stack:** LLVM 18 C++ API, IRBuilder, продолжение week 6.

---

## Files

- Modify: `include/solc/codegen/EvmIntrinsics.h` — расширить структуру
- Modify: `src/codegen/EvmIntrinsics.cpp`
- Modify: `src/codegen/CodeGen.cpp` — require, msg.*
- Create: `tests/fixtures/full_counter.sol` — эталонный Counter из CLAUDE.md
- Modify: `tests/test_codegen.cpp`

---

## Расширенный EvmIntrinsics

```cpp
struct EvmIntrinsics {
    llvm::Function* sload;      // i256 (i256)
    llvm::Function* sstore;     // void (i256, i256)
    llvm::Function* caller;     // i160 ()
    llvm::Function* callvalue;  // i256 ()
    llvm::Function* revert;     // void (i256, i256)
};
```

Декларация в `declareEvmIntrinsics(module)` — `ExternalLinkage`, никаких тел.

---

## Tasks

### Task 1: Extend EvmIntrinsics + declarations
- [ ] Добавить `caller`, `callvalue`, `revert`.
- [ ] Тест: `CodeGen.AllIntrinsicsDeclared` — в модуле 5 объявлений с нужными типами.
- [ ] Commit: `feat(codegen): declare caller/callvalue/revert intrinsics`

### Task 2: msg.sender
- [ ] `emitExpr(MemberExpr)` — если base=IdentExpr("msg") & member=="sender" → `CreateCall(intr.caller)`.
- [ ] Тип результата: i160 — совпадает с address.
- [ ] Тест: `address x = msg.sender;` → `%c = call i160 @evm.caller()` + store в i160 alloca.
- [ ] Commit: `feat(codegen): msg.sender → evm.caller`

### Task 3: msg.value
- [ ] Member "value" → `CreateCall(intr.callvalue)` (i256).
- [ ] Тест: `uint256 v = msg.value;` → эмит call.
- [ ] Commit: `feat(codegen): msg.value → evm.callvalue`

### Task 4: require — revert блок
- [ ] Эмит `require(cond)`:
  - Текущий блок → cond = `emitExpr(cond)`.
  - Создать `ok` и `revert` блоки.
  - `CondBr(cond, ok, revert)`.
  - В revert: `CreateCall(intr.revert, {ConstantInt i256 0, ConstantInt i256 0})`, затем `CreateUnreachable()`.
  - SetInsertPoint(ok).
- [ ] Тест: IR функции с require содержит:
  - блок с `br i1 %..., label %ok, label %revert`
  - блок `revert: call void @evm.revert(i256 0, i256 0); unreachable`
  - блок `ok:` продолжение кода
- [ ] Commit: `feat(codegen): require with revert block`

### Task 5: Полный Counter end-to-end
- [ ] `tests/fixtures/full_counter.sol` — буквально то, что в CLAUDE.md.
- [ ] Тест `CodeGen.FullCounter`:
  - Запускает пайплайн
  - `verifyModule` — no errors
  - В IR присутствуют: `call i160 @evm.caller`, `call i256 @evm.sload(i256 0)`, `call void @evm.sstore(i256 0, ...)`, `call void @evm.revert`
  - Функции `@increment` и `@isOwner` объявлены с правильными типами
- [ ] Commit: `test(codegen): full Counter compiles and verifies`

### Task 6: Интеграционный тест с `llvm-as`
- [ ] `scripts/run_llvm_as.sh`:
  ```bash
  #!/usr/bin/env bash
  set -e
  "$1" --input "$2" --emit-ir --output "$3"
  llvm-as "$3" -o "$3.bc"
  ```
- [ ] CMake: add_test, который запускает скрипт на full_counter.sol.
- [ ] Commit: `test(integration): llvm-as validates Counter IR`

### Task 7: Дополнительные фикстуры
- [ ] `simple_add.sol`: контракт с функцией `function add(uint256 a, uint256 b) public returns (uint256) { return a + b; }`.
- [ ] `with_require.sol`: функция только с require + return.
- [ ] `only_storage.sol`: несколько state vars, функция-аксессор.
- [ ] По тесту integration на каждую: `llvm-as` должен принять.
- [ ] Commit: `test(integration): additional fixtures pass llvm-as`

### Task 8: (Опционально) проверка сигнатур intrinsics через FileCheck-стайл
- [ ] Дописать в test_codegen helper, который извлекает IR-текст через `raw_string_ostream` и проверяет regex'ы на сигнатуры.
- [ ] Тесты: `@evm.sload` имеет сигнатуру `i256 (i256)`, и т.д.
- [ ] Commit: `test(codegen): intrinsic signatures asserted`

---

## Критерий готовности недели 7

- [ ] Полный `Counter.sol` из CLAUDE.md компилируется в `.ll`
- [ ] `llvm-as counter.ll` возвращает exit 0
- [ ] Все 5 EVM intrinsics объявлены в модуле
- [ ] `require(false)` в IR: CondBr + revert-блок с `unreachable`
- [ ] ≥ 4 дополнительные фикстуры тоже проходят `llvm-as`
- [ ] Интеграционный тест в `ctest` зелёный
