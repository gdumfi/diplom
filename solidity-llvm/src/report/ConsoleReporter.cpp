#include "solc/report/ConsoleReporter.h"

namespace solc::report {

void ConsoleReporter::report(const Findings& findings, std::ostream& os) {
    if (findings.empty()) {
        os << "Проблем не найдено.\n";
        return;
    }
    for (const auto& f : findings) {
        os << "[" << f.severity << "] " << f.rule;
        if (f.symConfirmed) os << " [SYM-CONFIRMED]";   // символьно подтверждён KLEE (ров)
        os << " — функция '" << f.function << "'";
        if (f.line > 0) os << ", строка " << f.line;   // куда смотреть разработчику
        os << "\n    " << f.message << "\n";
    }
    os << "\nНайдено находок: " << findings.size() << "\n";
}

}  // namespace solc::report
