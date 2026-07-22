#pragma once
#include <ostream>

#include "solc/report/Finding.h"

namespace solc::report {

// печатает находки в виде JSON-массива объектов (для машинной обработки, --json)
class JsonReporter {
public:
    // печатает JSON-массив находок в os; пустой список даёт "[]"
    void report(const Findings& findings, std::ostream& os);
};

}  // namespace solc::report
