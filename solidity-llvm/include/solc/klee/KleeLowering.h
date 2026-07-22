#pragma once
#include <memory>

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Module.h>

namespace solc::klee {

// понижение размеченного EVM-IR (наш codegen) в самодостаточный KLEE-исполнимый
// модуль (K1, §6/§4.3). переиспользование KLEE — защищаемый компонент диплома:
//   - storage (addrspace(1), @scsa.map_elem) -> реальная глобальная память;
//   - контекст (@evm.msg_sender/msg_value/tx_origin/block_timestamp) -> символьные
//     глобали (klee_make_symbolic в харнессе);
//   - @evm.revert -> klee_silent_exit (путь отсекается, откат транзакции);
//   - @scsa.external_call -> символьный флаг успеха (fork реентрантного callback —
//     K1.1, пока заглушка «успех»);
//   - харнесс @main: символизирует контекст+аргументы, зовёт уязвимую функцию.
// после понижения ни одного внешнего @evm.*/@scsa.* символа не остаётся.
// какой класс уязвимости символьно подтверждаем
enum class Mode {
    Reentrancy,     // fork callback глубины 1 + оракул total_sent > баланс
    AccessControl,  // оракул: атакующий msg.sender != owner достигает привилегированной записи
};

class KleeLowering {
public:
    // строит новый модуль в том же LLVMContext, что и src (llvm::CloneModule).
    // entryFn — имя уязвимой функции, которую вызывает харнесс.
    // effectSlot — для Reentrancy: слот storage-записи после вызова (порог оракула, баланс);
    //   для AccessControl: привилегированный слот (privSlot), запись в который — эффект.
    //   -1 = сумма всех (fallback для reentrancy).
    // mode — класс уязвимости (см. Mode). ownerSlot — слот owner (только AccessControl):
    //   оракул срабатывает, если запись в privSlot достижима при msg.sender != storage[ownerSlot].
    // возвращает nullptr, если функция не найдена или модуль не прошёл верификацию.
    std::unique_ptr<llvm::Module> lower(const llvm::Module& src, llvm::StringRef entryFn,
                                        int effectSlot = -1, Mode mode = Mode::Reentrancy,
                                        int ownerSlot = -1);
};

}  // namespace solc::klee
