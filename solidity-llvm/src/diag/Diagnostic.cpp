#include "solc/diag/Diagnostic.h"

namespace solc {

void DiagnosticEngine::error(SourceLoc loc, std::string msg) {
    errors_.push_back({loc, std::move(msg)});
}

}  // namespace solc
