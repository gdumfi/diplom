#pragma once
#include <llvm/IR/Module.h>

#include "solc/report/Finding.h"

namespace solc::analyzer {

// детектор reentrancy поверх размеченного LLVM IR (см. AD-4, §5.1).
// ищет нарушение checks-effects-interactions: запись в storage (addrspace 1),
// достижимую ПОСЛЕ внешнего вызова @scsa.external_call на каком-либо пути CFG.
// номер строки берётся с !dbg storage-записи (эффекта), иначе с внешнего вызова.
// nonReentrant-guard (require(!locked); locked=true; ...call...) распознаётся и
// понижает находку до LOW (пометка guarded) вместо HIGH — детали в .cpp.
class ReentrancyDetector {
public:
    // анализирует все функции модуля, возвращает список находок
    report::Findings analyze(const llvm::Module& m);
};

}  // namespace solc::analyzer
