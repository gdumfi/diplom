#include "solc/report/SarifReporter.h"

namespace solc::report {
namespace {

std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:   o += c;      break;
        }
    }
    return o;
}

// severity -> уровень SARIF
const char* sarifLevel(const std::string& sev) {
    if (sev == "HIGH")   return "error";
    if (sev == "MEDIUM") return "warning";
    return "note";   // LOW и прочее
}

}  // namespace

void SarifReporter::report(const Findings& findings, const std::string& filePath, std::ostream& os) {
    os << "{\n";
    os << "  \"$schema\": \"https://json.schemastore.org/sarif-2.1.0.json\",\n";
    os << "  \"version\": \"2.1.0\",\n";
    os << "  \"runs\": [\n    {\n";
    os << "      \"tool\": { \"driver\": { \"name\": \"SCSA\", \"informationUri\": "
          "\"https://github.com/\", \"rules\": [] } },\n";
    os << "      \"results\": [";
    for (size_t i = 0; i < findings.size(); ++i) {
        const auto& f = findings[i];
        if (i) os << ",";
        os << "\n        {\n";
        os << "          \"ruleId\": \"" << esc(f.rule) << "\",\n";
        os << "          \"level\": \"" << sarifLevel(f.severity) << "\",\n";
        os << "          \"message\": { \"text\": \"[" << esc(f.severity) << "] "
           << (f.symConfirmed ? "[SYM-CONFIRMED] " : "")
           << esc(f.function) << ": " << esc(f.message) << "\" },\n";
        os << "          \"properties\": { \"symConfirmed\": "
           << (f.symConfirmed ? "true" : "false") << " },\n";
        os << "          \"locations\": [ { \"physicalLocation\": { "
           << "\"artifactLocation\": { \"uri\": \"" << esc(filePath) << "\" }";
        if (f.line > 0)
            os << ", \"region\": { \"startLine\": " << f.line << " }";
        os << " } } ]\n";
        os << "        }";
    }
    os << "\n      ]\n    }\n  ]\n}\n";
}

}  // namespace solc::report
