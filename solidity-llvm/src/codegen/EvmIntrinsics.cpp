#include "solc/codegen/EvmIntrinsics.h"
#include <llvm/IR/DerivedTypes.h>

namespace solc::codegen {

EvmIntrinsics declareEvmIntrinsics(llvm::Module& m) {
    auto& ctx = m.getContext();
    auto* i160 = llvm::IntegerType::get(ctx, 160);
    auto* i256 = llvm::IntegerType::get(ctx, 256);
    auto* voidTy = llvm::Type::getVoidTy(ctx);

    EvmIntrinsics e;

    // void @evm.revert() — noreturn: control никогда не выходит из вызова
    auto* revertTy = llvm::FunctionType::get(voidTy, {}, false);
    e.revert = llvm::Function::Create(
        revertTy, llvm::Function::ExternalLinkage, "evm.revert", &m);
    e.revert->setDoesNotReturn();

    // i160 @evm.msg_sender()
    auto* msgSenderTy = llvm::FunctionType::get(i160, {}, false);
    e.msg_sender = llvm::Function::Create(
        msgSenderTy, llvm::Function::ExternalLinkage, "evm.msg_sender", &m);

    // i256 @evm.msg_value()
    auto* msgValueTy = llvm::FunctionType::get(i256, {}, false);
    e.msg_value = llvm::Function::Create(
        msgValueTy, llvm::Function::ExternalLinkage, "evm.msg_value", &m);

    // i160 @evm.tx_origin()
    auto* txOriginTy = llvm::FunctionType::get(i160, {}, false);
    e.tx_origin = llvm::Function::Create(
        txOriginTy, llvm::Function::ExternalLinkage, "evm.tx_origin", &m);

    // i256 @evm.block_timestamp()  — now / block.timestamp
    auto* tsTy = llvm::FunctionType::get(i256, {}, false);
    e.block_timestamp = llvm::Function::Create(
        tsTy, llvm::Function::ExternalLinkage, "evm.block_timestamp", &m);

    // i256 @evm.block_number()  — block.number (символьный для KLEE)
    e.block_number = llvm::Function::Create(
        tsTy, llvm::Function::ExternalLinkage, "evm.block_number", &m);

    // void @evm.selfdestruct(i160 recipient)
    auto* sdTy = llvm::FunctionType::get(voidTy, {i160}, false);
    e.self_destruct = llvm::Function::Create(
        sdTy, llvm::Function::ExternalLinkage, "evm.selfdestruct", &m);

    // i1 @scsa.external_call(i160 target, i256 value)
    // маркер для анализатора: управление уходит наружу и может вернуться (callback).
    // возвращает флаг успеха вызова (в MVP игнорируется вызывающим кодом)
    auto* extCallTy = llvm::FunctionType::get(
        llvm::Type::getInt1Ty(ctx), {i160, i256}, false);
    e.external_call = llvm::Function::Create(
        extCallTy, llvm::Function::ExternalLinkage, "scsa.external_call", &m);

    // ptr addrspace(1) @scsa.map_elem(i256 baseSlot, i256 key)
    auto* storagePtrTy = llvm::PointerType::get(ctx, /*addrspace storage=*/1);
    auto* mapElemTy = llvm::FunctionType::get(storagePtrTy, {i256, i256}, false);
    e.map_elem = llvm::Function::Create(
        mapElemTy, llvm::Function::ExternalLinkage, "scsa.map_elem", &m);

    // void @scsa.opaque_store() — маркер непрозрачной записи в storage-агрегат
    auto* opaqueTy = llvm::FunctionType::get(voidTy, {}, false);
    e.opaque_store = llvm::Function::Create(
        opaqueTy, llvm::Function::ExternalLinkage, "scsa.opaque_store", &m);

    return e;
}

}  // namespace solc::codegen
