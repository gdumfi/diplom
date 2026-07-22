#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace solc {

// позиция в исходнике, 1-based для line и col
struct SourceLoc {
    uint32_t line = 1;
    uint32_t col  = 1;
};

// одна диагностика: где и что
struct Diagnostic {
    SourceLoc loc;
    std::string message;
};

class DiagnosticEngine {
public:
    void error(SourceLoc loc, std::string msg);
    const std::vector<Diagnostic>& errors() const { return errors_; }
    bool hasErrors() const { return !errors_.empty(); }
private:
    std::vector<Diagnostic> errors_;
};

}  // namespace solc
