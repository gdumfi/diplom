#pragma once
#include <llvm/IR/Module.h>

namespace solc::analyzer {

// подготовка IR к анализу: mem2reg (promote alloca -> SSA). без этого локальные
// переменные идут через alloca/store/load, и def-use трассировка детекторов
// (msg.sender/tx.origin/флаг успеха вызова) рвётся на копии в локаль. см. CLAUDE §5.1.
void promoteLocals(llvm::Module& m);

}  // namespace solc::analyzer
