#pragma once
#include <ostream>

#include "solc/report/Finding.h"

namespace solc::report {

// печатает находки в человекочитаемом виде (для CLI `sca audit`)
class ConsoleReporter {
public:
    // печатает список находок и итоговую строку в os
    void report(const Findings& findings, std::ostream& os);
};

}  // namespace solc::report
