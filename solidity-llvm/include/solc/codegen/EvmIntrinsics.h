#pragma once
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>

namespace solc::codegen {

struct EvmIntrinsics {
    // storage теперь моделируется через addrspace(1) load/store в CodeGen,
    // а не через sload/sstore — поэтому этих функций тут больше нет (см. AD-3)
    llvm::Function* revert        = nullptr;
    llvm::Function* msg_sender    = nullptr;
    llvm::Function* msg_value     = nullptr;
    llvm::Function* tx_origin     = nullptr;
    llvm::Function* block_timestamp = nullptr;   // now / block.timestamp
    llvm::Function* block_number    = nullptr;   // block.number (символьный для KLEE, §4.3)
    llvm::Function* self_destruct   = nullptr;   // selfdestruct(recipient)
    // маркер внешнего вызова для анализатора: @scsa.external_call(i160 target, i256 value) -> i1
    llvm::Function* external_call = nullptr;
    // адрес элемента mapping: @scsa.map_elem(i256 baseSlot, i256 key) -> ptr addrspace(1).
    // моделирует keccak-адресацию storage без реального хеша — для анализа важно лишь,
    // что это указатель в storage (addrspace 1), привязанный к базовому слоту mapping
    llvm::Function* map_elem = nullptr;
    // маркер непрозрачной записи в storage-агрегат (поле struct / элемент массива),
    // которую модель НЕ различает: @scsa.opaque_store(). позволяет детектору честно
    // предупредить о возможной reentrancy вместо тихого ложного негатива
    llvm::Function* opaque_store = nullptr;
};

EvmIntrinsics declareEvmIntrinsics(llvm::Module& m);

}  // namespace solc::codegen
