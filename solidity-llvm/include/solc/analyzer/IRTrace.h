#pragma once
#include <string>
#include <unordered_set>
#include <vector>

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

// общие примитивы def-use трассировки для детекторов (устраняют дублирование).
// обход идёт через приведения ширины (trunc/zext), которыми codegen оборачивает
// значения; в память/вызовы не заходим — только вычислительная цепочка.
namespace solc::analyzer::trace {

// слот из метаданного !scsa.storage !{i64 slot}, либо -1 (работает для load и store)
int storageSlotOf(const llvm::Instruction* inst);

// восходит ли значение к вызову функции с именем calleeName (напр. evm.msg_sender)
bool reachesCall(const llvm::Value* v, llvm::StringRef calleeName);

// восходит ли значение к чтению СКАЛЯРНОГО storage (addrspace 1, не mapping-элемент);
// при успехе пишет слот. mapping-элементы исключены — это per-user, не owner-скаляр
bool reachesStorageLoad(const llvm::Value* v, int& slot);

// то же, но включая mapping-элементы (для cross-function reentrancy: value вызова
// часто вычислен из mapping-баланса balances[msg.sender])
bool reachesStorageLoadAny(const llvm::Value* v, int& slot);

// то же + проходит арифметику (add/sub/mul/div/rem): value вызова часто вычислен как
// balance*price/1e18 — обычная (только trunc/zext) трасса на этом рвётся
bool reachesStorageLoadDeep(const llvm::Value* v, int& slot);

// первая инструкция блока — call @evm.revert (ветка require.fail)
bool isRevertBlock(const llvm::BasicBlock* bb);

// гейтит ли значение revert: является ли оно (возможно, через && / || = and/or i1)
// условием ветки, одна из которых — revert-блок. нужно, чтобы отличить проверку,
// реально откатывающую транзакцию, от вычисления флага без ветвления
bool gatesRevert(const llvm::Value* v);

// есть ли в функции проверка с msg.sender, реально гейтящая revert — т.е. функция
// защищена контролем прав. используется детекторами selfdestruct/withdrawal
bool hasMsgSenderGate(const llvm::Function& fn);

// ABI-типы параметров функции из метаданного !scsa.abi (реальные типы из solc-AST:
// uint128/bytes32/address[]/…), нужны PoC для ВЕРНОГО селектора. пусто, если метки
// нет (legacy-фронтенд) — тогда детектор падает назад к выводу из llvm-типа
std::vector<std::string> functionParamAbiTypes(const llvm::Function& fn);

// --- распознавание OZ-абстракции гвардов (Context/_msgSender, Ownable/_checkOwner) ---
// функции, возвращающие msg.sender (напр. OZ Context._msgSender) — чтобы узнавать
// msg.sender за обёрткой (`_msgSender()` вместо прямого msg.sender)
std::unordered_set<const llvm::Function*> collectMsgSenderFns(const llvm::Module& m);
// восходит ли v к msg.sender: напрямую ИЛИ через вызов msg.sender-возвращающей функции
bool reachesMsgSenderX(const llvm::Value* v,
                       const std::unordered_set<const llvm::Function*>& msgSenderFns);
// функция — auth-гвард: условный revert по сравнению с msg.sender (напр. Ownable._checkOwner:
// `if (owner() != _msgSender()) revert`). нужно, чтобы вызов такой функции считался проверкой прав
bool isAuthGuardFn(const llvm::Function& fn,
                   const std::unordered_set<const llvm::Function*>& msgSenderFns);

}  // namespace solc::analyzer::trace
