#pragma once
#include <ostream>
#include <string>

#include "solc/report/Finding.h"

namespace solc::report {

// вывод находок в формате SARIF 2.1.0 — для интеграции с CI/GitHub code scanning.
class SarifReporter {
public:
    // filePath — путь к анализируемому файлу (для локаций в SARIF)
    void report(const Findings& findings, const std::string& filePath, std::ostream& os);
};

}  // namespace solc::report
