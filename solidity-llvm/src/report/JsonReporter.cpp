#include "solc/report/JsonReporter.h"

#include <string>

namespace solc::report {

namespace {

// экранируем строку для JSON вручную: кавычка, обратный слэш, перевод строки.
// русский текст в UTF-8 остаётся как есть (без \uXXXX).
std::string escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

}  // namespace

void JsonReporter::report(const Findings& findings, std::ostream& os) {
    // пустой список -> компактный "[]"
    if (findings.empty()) {
        os << "[]\n";
        return;
    }

    os << "[\n";
    for (std::size_t i = 0; i < findings.size(); ++i) {
        const Finding& f = findings[i];
        os << "  {\n"
           << "    \"rule\": \""        << escape(f.rule)     << "\",\n"
           << "    \"severity\": \""    << escape(f.severity) << "\",\n"
           << "    \"function\": \""    << escape(f.function) << "\",\n"
           << "    \"line\": "          << f.line             << ",\n"
           << "    \"storageSlot\": "   << f.storageSlot      << ",\n"
           << "    \"symConfirmed\": "  << (f.symConfirmed ? "true" : "false") << ",\n"
           << "    \"priorityScore\": " << f.priorityScore    << ",\n"
           << "    \"message\": \""     << escape(f.message)  << "\"\n"
           << "  }" << (i + 1 < findings.size() ? "," : "") << "\n";
    }
    os << "]\n";
}

}  // namespace solc::report
