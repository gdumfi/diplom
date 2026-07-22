#include "solc/driver/Driver.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include "solc/analyzer/AccessControlDetector.h"
#include "solc/analyzer/AnalysisPrep.h"
#include "solc/analyzer/PatternDetectors.h"
#include "solc/analyzer/ReentrancyDetector.h"
#include "solc/codegen/AstJsonLowering.h"
#include "solc/codegen/CodeGen.h"
#include "solc/diag/Diagnostic.h"
#include "solc/driver/EtherscanFetch.h"
#include "solc/driver/SolcRunner.h"
#include "solc/klee/KleeLowering.h"
#include "solc/sema/Inheritance.h"
#include "solc/lexer/Lexer.h"
#include "solc/parser/Parser.h"
#include "solc/report/ConsoleReporter.h"
#include "solc/report/ContextPackWriter.h"
#include "solc/report/InvariantGen.h"
#include "solc/report/JsonReporter.h"
#include "solc/report/PocGenerator.h"
#include "solc/report/SarifReporter.h"
#include "solc/sema/Sema.h"

// имя единицы компиляции (ключ в solc AST): базовое имя входного файла
static std::string unitNameOf(const std::string& path) {
    if (path.empty()) return "input.sol";
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

namespace {

// символ — часть идентификатора (для границ слова import)
static bool isIdentCh(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
}

// извлечь спецификаторы модулей из import-выражений исходника. комментарии и обычные
// строковые литералы пропускаются; в каждом import берётся первый строковый литерал —
// это и есть путь модуля (форма `import "x"`, `import {a} from "x"`, `import * as n from "x"`)
static std::vector<std::string> scanImportPaths(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0, n = s.size();
    while (i < n) {
        if (s[i] == '/' && i + 1 < n && s[i + 1] == '/') {           // строчный комментарий
            i += 2; while (i < n && s[i] != '\n') ++i; continue;
        }
        if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {           // блочный комментарий
            i += 2; while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) ++i; i += 2; continue;
        }
        if (s[i] == '"' || s[i] == '\'') {                           // строка вне import — пропустить
            char q = s[i++]; while (i < n && s[i] != q) { if (s[i] == '\\' && i + 1 < n) ++i; ++i; }
            if (i < n) ++i; continue;
        }
        if (s.compare(i, 6, "import") == 0 && (i == 0 || !isIdentCh(s[i - 1])) &&
            (i + 6 >= n || !isIdentCh(s[i + 6]))) {                  // ключевое слово import
            i += 6;
            std::string path;
            while (i < n && s[i] != ';') {
                if (s[i] == '/' && i + 1 < n && s[i + 1] == '/') { i += 2; while (i < n && s[i] != '\n') ++i; continue; }
                if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') { i += 2; while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) ++i; i += 2; continue; }
                if (s[i] == '"' || s[i] == '\'') {                   // первый строковый литерал = путь
                    char q = s[i++];
                    while (i < n && s[i] != q) { if (s[i] == '\\' && i + 1 < n) { path += s[i + 1]; i += 2; } else path += s[i++]; }
                    if (i < n) ++i;
                    break;
                }
                ++i;
            }
            if (!path.empty()) out.push_back(path);
            continue;
        }
        ++i;
    }
    return out;
}

// прочитать файл целиком; пусто, если не открылся
static std::string readFileOr(const std::filesystem::path& p) {
    std::ifstream in(p);
    if (!in) return {};
    std::stringstream ss; ss << in.rdbuf(); return ss.str();
}

// одно remap-правило: префикс импорта → абсолютный каталог-цель (без хвостового '/')
struct RemapRule { std::string prefix; std::string absTarget; };

// нормализованный абсолютный каталог БЕЗ хвостового '/' (иначе remap даёт `dir//file` ≠ ключа)
static std::string normDirStr(const std::filesystem::path& p) {
    std::string s = p.lexically_normal().string();
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

// разобрать remappings.txt (foundry/hardhat): по строке `from=to`, `#`/`//` — комментарий;
// to трактуется относительно root. context-remapping (`ctx:from=to`) пропускаем
static void loadRemappingsTxt(const std::string& txt, const std::filesystem::path& root,
                              std::vector<RemapRule>& rules) {
    namespace fs = std::filesystem;
    std::stringstream ss(txt); std::string line;
    while (std::getline(ss, line)) {
        size_t a = line.find_first_not_of(" \t\r");
        if (a == std::string::npos || line[a] == '#' ||
            (line[a] == '/' && a + 1 < line.size() && line[a + 1] == '/')) continue;
        size_t eq = line.find('=', a);
        if (eq == std::string::npos) continue;
        std::string from = line.substr(a, eq - a);
        std::string to = line.substr(eq + 1);
        size_t z = to.find_last_not_of(" \t\r"); if (z != std::string::npos) to = to.substr(0, z + 1);
        if (from.find(':') != std::string::npos || from.empty() || to.empty()) continue;
        rules.push_back({from, normDirStr(root / to)});
    }
}

// извлечь remappings-массив из foundry.toml (`remappings = ["from=to", ...]`, возможно многострочно)
static void loadFoundryToml(const std::string& txt, const std::filesystem::path& root,
                            std::vector<RemapRule>& rules) {
    size_t k = txt.find("remappings");
    if (k == std::string::npos) return;
    size_t lb = txt.find('[', k); size_t rb = (lb == std::string::npos) ? std::string::npos : txt.find(']', lb);
    if (lb == std::string::npos || rb == std::string::npos) return;
    std::string arr = txt.substr(lb + 1, rb - lb - 1);
    size_t i = 0;
    while (i < arr.size()) {
        char c = arr[i++];
        if (c == '"' || c == '\'') {
            std::string s; while (i < arr.size() && arr[i] != c) s += arr[i++];
            if (i < arr.size()) ++i;
            size_t eq = s.find('=');
            if (eq == std::string::npos) continue;
            std::string from = s.substr(0, eq), to = s.substr(eq + 1);
            if (from.find(':') != std::string::npos || from.empty() || to.empty()) continue;
            rules.push_back({from, normDirStr(root / to)});
        }
    }
}

// собрать транзитивное замыкание импортов локального файла (реальные репозитории: OZ и пр.).
// заполняет files (ключ = абсолютный нормализованный путь → контент) и remappings.
// правила берутся из remappings.txt / foundry.toml / авто-lib (foundry) / node_modules (hardhat).
// возвращает ключ целевого файла (primaryUnit). одиночная фикстура без import-ов → ровно один файл.
static std::string gatherLocalSources(const std::string& inputPath,
                                      std::vector<std::pair<std::string, std::string>>& files,
                                      std::vector<std::string>& remappings) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path target = fs::absolute(inputPath, ec).lexically_normal();

    // корень проекта = ближайший предок цели с маркером сборки
    fs::path root;
    for (fs::path d = target.parent_path(); ; d = d.parent_path()) {
        if (fs::exists(d / "remappings.txt", ec) || fs::exists(d / "foundry.toml", ec) ||
            fs::exists(d / "node_modules", ec) || fs::exists(d / ".git", ec)) { root = d; break; }
        if (d == d.root_path() || d.empty()) break;
    }
    if (root.empty()) root = target.parent_path();
    fs::path nmBase;
    if (fs::exists(root / "node_modules", ec)) nmBase = (root / "node_modules").lexically_normal();

    // явные remap-правила: remappings.txt имеет приоритет над foundry.toml
    std::vector<RemapRule> rules;
    if (fs::exists(root / "remappings.txt", ec)) loadRemappingsTxt(readFileOr(root / "remappings.txt"), root, rules);
    if (fs::exists(root / "foundry.toml", ec))   loadFoundryToml(readFileOr(root / "foundry.toml"), root, rules);
    // авто-remapping из lib/ (foundry без файла remappings): <dir>/=lib/<dir>/src|/
    if (fs::is_directory(root / "lib", ec)) {
        for (const auto& de : fs::directory_iterator(root / "lib", ec)) {
            if (!de.is_directory(ec)) continue;
            std::string name = de.path().filename().string();
            fs::path tgt = fs::exists(de.path() / "src", ec) ? de.path() / "src" : de.path();
            rules.push_back({name + "/", normDirStr(tgt)});
        }
    }
    // дедуп по префиксу (первое правило выигрывает), затем longest-prefix первым
    std::vector<RemapRule> uniq;
    for (const auto& r : rules) {
        bool dup = false; for (const auto& u : uniq) if (u.prefix == r.prefix) { dup = true; break; }
        if (!dup) uniq.push_back(r);
    }
    std::sort(uniq.begin(), uniq.end(),
              [](const RemapRule& a, const RemapRule& b) { return a.prefix.size() > b.prefix.size(); });

    std::set<std::string> remapSet, seen;
    std::vector<fs::path> stack{target};
    while (!stack.empty()) {
        fs::path cur = stack.back(); stack.pop_back();
        std::string key = cur.string();
        if (!seen.insert(key).second) continue;
        std::ifstream in(cur);
        if (!in) continue;   // отсутствующий файл — пропуск (solc сам ругнётся, если он реально нужен)
        std::stringstream ss; ss << in.rdbuf();
        std::string content = ss.str();
        files.emplace_back(key, content);
        for (const auto& spec : scanImportPaths(content)) {
            fs::path resolved;
            if (spec.rfind("./", 0) == 0 || spec.rfind("../", 0) == 0) {
                resolved = (cur.parent_path() / spec).lexically_normal();     // относительный
            } else {
                bool matched = false;
                for (const auto& r : uniq)                                    // явное правило (longest-prefix)
                    if (spec.compare(0, r.prefix.size(), r.prefix) == 0) {
                        // остаток может начинаться с '/' (префикс без хвостового '/', как
                        // `@openzeppelin/contracts=lib/…/contracts/`) — иначе fs::path/'/x'
                        // сочтёт его абсолютным и заменит каталог-цель. подражаем строковой
                        // подстановке remapping (solc/foundry) — обрезаем ведущий '/'
                        std::string rest = spec.substr(r.prefix.size());
                        while (!rest.empty() && rest.front() == '/') rest.erase(rest.begin());
                        resolved = (fs::path(r.absTarget) / rest).lexically_normal();
                        // хвостовой '/' цели ЗЕРКАЛИТ префикс: solc не схлопывает '//', а делает
                        // строковую подстановку — иначе `target/`+`/rest` = `//` ≠ ключа sources
                        remapSet.insert(r.prefix + "=" + r.absTarget +
                                        (r.prefix.back() == '/' ? "/" : ""));
                        matched = true; break;
                    }
                if (!matched && !nmBase.empty()) {                            // hardhat: node_modules-пакет
                    resolved = (nmBase / spec).lexically_normal();
                    size_t p1 = spec.find('/');
                    if (p1 != std::string::npos) {
                        std::string prefix;
                        if (spec[0] == '@') { size_t p2 = spec.find('/', p1 + 1);
                            if (p2 != std::string::npos) prefix = spec.substr(0, p2); }   // scoped @a/b
                        else prefix = spec.substr(0, p1);
                        if (!prefix.empty())
                            remapSet.insert(prefix + "/=" + normDirStr(nmBase / prefix) + "/");
                    }
                    matched = true;
                }
                if (!matched) resolved = fs::path(spec).lexically_normal();
            }
            if (!seen.count(resolved.string())) stack.push_back(resolved);
        }
    }
    remappings.assign(remapSet.begin(), remapSet.end());
    return target.string();
}

}  // namespace

namespace solc {

// печатаем диагностики в stderr — единый формат для пользователя
static void printDiagnostics(const DiagnosticEngine& diag) {
    for (const auto& e : diag.errors()) {
        std::cerr << "error: " << e.loc.line << ":" << e.loc.col
                  << ": " << e.message << "\n";
    }
}

// структурный код причины SKIP для батч-воронки (docs/ROBUSTNESS-PLAN §2). печатаем
// тегом в stderr — hunt_batch.sh бакетит по коду, а не по grep-эвристике
static void emitSkip(const std::string& code, const std::string& detail) {
    std::cerr << "SCSA_SKIP:" << code << " " << detail << "\n";
}

// версия solc < 0.8 (0.4–0.7)? тогда контракт вне охвата AstJsonLowering (0.8-AST) — честный
// SKIP до вызова solc (ROBUSTNESS-PLAN §4, фикс 1d)
static bool isPre08(const std::string& ver) {
    return ver.rfind("0.4.", 0) == 0 || ver.rfind("0.5.", 0) == 0 ||
           ver.rfind("0.6.", 0) == 0 || ver.rfind("0.7.", 0) == 0 ||
           ver.rfind("0.3.", 0) == 0 || ver.rfind("0.2.", 0) == 0 || ver.rfind("0.1.", 0) == 0;
}

// классифицируем ошибку solc в код бакета (ROBUSTNESS-PLAN §2, слой A)
static std::string classifySolc(const std::string& err) {
    auto has = [&](const char* s) { return err.find(s) != std::string::npos; };
    if (has("образа") || has("не дал вывода"))            return "A1";  // нет docker-образа версии
    if (has("requires different compiler version"))       return "A2";  // не та версия solc
    return "A3";  // прочая ошибка компиляции (settings/строгость/парсер)
}

// ранг severity для порога --min-severity
static int sevRank(const std::string& s) {
    if (s == "HIGH") return 3;
    if (s == "MEDIUM") return 2;
    if (s == "LOW") return 1;
    return 0;
}

// приоритет триажа находки: деньги×severity + бонус за символьное подтверждение.
// веса «денег» по разделу 7 (access-control #1 $953M, reentrancy — легенда, и т.д.).
// KLEE-подтверждение доминирует: доказанный эксплойт ценнее паттерна (ров, AD-7)
static int scoreFinding(const report::Finding& f) {
    int score = sevRank(f.severity) * 100;   // severity — основной множитель
    int money = 0;                            // денежный вес класса (раздел 7)
    if (f.rule == "access-control")      money = 50;   // #1, $953M
    else if (f.rule == "arbitrary-call") money = 48;   // Multichain $126M/Socket/LiFi — доминирует хаки 0.8
    else if (f.rule == "reentrancy")     money = 40;   // легенда/демонстрация метода
    else if (f.rule == "price-manipulation") money = 45;  // топ-оплачиваемый (flash-loan)
    else if (f.rule == "read-only-reentrancy") money = 25;  // Curve-2023, мало дубликатов
    else if (f.rule == "delegatecall")   money = 35;   // SWC-112, чужой код
    else if (f.rule == "selfdestruct")   money = 30;   // SWC-106
    else if (f.rule == "tx-origin")      money = 15;
    else if (f.rule == "weak-randomness") money = 12;
    else if (f.rule == "unchecked-call") money = 5;
    score += money;
    if (f.symConfirmed) score += 1000;        // подтверждённый KLEE — наверх
    return score;
}

// прогнать все детекторы на одном модуле (после mem2reg), вернуть находки
static report::Findings runDetectors(llvm::Module& mod) {
    analyzer::promoteLocals(mod);   // локали -> SSA для def-use трассировки
    report::Findings findings;
    analyzer::ReentrancyDetector rDet;
    auto r = rDet.analyze(mod);
    findings.insert(findings.end(), r.begin(), r.end());
    analyzer::AccessControlDetector acDet;
    auto ac = acDet.analyze(mod);
    findings.insert(findings.end(), ac.begin(), ac.end());
    analyzer::TxOriginDetector txDet;
    auto tx = txDet.analyze(mod);
    findings.insert(findings.end(), tx.begin(), tx.end());
    analyzer::UncheckedCallDetector ucDet;
    auto uc = ucDet.analyze(mod);
    findings.insert(findings.end(), uc.begin(), uc.end());
    analyzer::SelfdestructDetector sdDet;
    auto sd = sdDet.analyze(mod);
    findings.insert(findings.end(), sd.begin(), sd.end());
    analyzer::DelegatecallDetector dcDet;
    auto dc = dcDet.analyze(mod);
    findings.insert(findings.end(), dc.begin(), dc.end());
    analyzer::WeakRandomnessDetector wrDet;
    auto wr = wrDet.analyze(mod);
    findings.insert(findings.end(), wr.begin(), wr.end());
    analyzer::OraclePriceDetector opDet;
    auto op = opDet.analyze(mod);
    findings.insert(findings.end(), op.begin(), op.end());
    analyzer::OracleStalenessDetector osDet;
    auto os = osDet.analyze(mod);
    findings.insert(findings.end(), os.begin(), os.end());
    analyzer::ArbitraryCallDetector arbDet;
    auto arb = arbDet.analyze(mod);
    findings.insert(findings.end(), arb.begin(), arb.end());
    analyzer::SlippageDetector slipDet;
    auto slip = slipDet.analyze(mod);
    findings.insert(findings.end(), slip.begin(), slip.end());
    return findings;
}

int Driver::run(const CompileOptions& opts) {
    // исходные файлы (путь→контент): один для --input, возможно несколько для --address
    std::vector<std::pair<std::string, std::string>> files;
    std::vector<std::string> remappings;        // remappings из Etherscan (для import-ов)
    std::string targetContract;                 // цель для --address (multi-file выбор)
    std::string primaryUnit = unitNameOf(opts.inputPath);
    std::string solcVersion = opts.solcVersion; // для --address берём версию из Etherscan

    if (!opts.address.empty()) {
        // --address: тянем верифицированный исходник с Etherscan (адрес → анализ одной командой)
        std::string name, err, ver;
        if (!driver::fetchVerifiedSources(opts.address, opts.chain, name, ver, files, remappings, err)) {
            std::cerr << "error: Etherscan fetch failed: " << err << "\n";
            emitSkip("A0", "fetch: " + err);   // не получен исходник (сеть/ключ/не верифицирован)
            return 1;
        }
        targetContract = name;
        if (!ver.empty()) solcVersion = ver;    // точная версия solc контракта
        size_t total = 0; for (auto& f : files) total += f.second.size();
        std::cerr << "получен исходник " << name << " (" << opts.address << ", chain "
                  << opts.chain << "), файлов: " << files.size() << ", " << total
                  << " байт, solc " << solcVersion << "\n";
    } else {
        // файл + транзитивное замыкание его импортов; remappings из node_modules.
        // одиночная фикстура без import-ов → ровно один файл (прежнее поведение)
        std::string primary = gatherLocalSources(opts.inputPath, files, remappings);
        if (files.empty()) {
            std::cerr << "error: cannot open '" << opts.inputPath << "'\n";
            return 1;
        }
        primaryUnit = primary;
    }

    DiagnosticEngine diag;

    // владельцы контекстов/фронтендов держим живыми до конца run() (модули ссылаются на них)
    std::vector<std::unique_ptr<codegen::CodeGen>> cgs;        // legacy-путь
    codegen::AstJsonLowering lowering(diag);                   // solc-путь (владеет контекстами)
    std::vector<std::unique_ptr<llvm::Module>> mods;

    if (opts.legacy) {
        // старый рукописный фронтенд (0.4.x-подмножество) — только для регресса/сравнения.
        // multi-file не поддерживает — конкатенируем файлы
        std::string source;
        for (auto& f : files) { source += f.second; source += "\n"; }
        Lexer  lex(source, diag);
        Parser p(lex, diag);
        auto contracts = p.parseSourceUnit();
        if (contracts.empty() || diag.hasErrors()) { printDiagnostics(diag); return 1; }
        contracts = sema::mergeInheritance(std::move(contracts), diag);
        for (auto& c : contracts) {
            sema::SemanticAnalyzer sa(diag);
            if (!sa.analyze(c) || diag.hasErrors()) { printDiagnostics(diag); return 1; }
            auto cg  = std::make_unique<codegen::CodeGen>(diag);
            auto mod = cg->generate(c);
            if (!mod || diag.hasErrors()) { printDiagnostics(diag); return 1; }
            cgs.push_back(std::move(cg));
            mods.push_back(std::move(mod));
        }
    } else {
        // scope-guard: pre-0.8 (0.4–0.7) вне охвата AstJsonLowering — честный SKIP до solc
        if (isPre08(solcVersion)) {
            std::cerr << "error: контракт на solc " << solcVersion << " — вне охвата (нужен 0.8.x)\n";
            emitSkip("C", "pre-0.8 (" + solcVersion + ")");
            return 1;
        }
        // боевой фронтенд: solc --standard-json (разрешённый AST) -> AstJsonLowering.
        // резолв имён/типов/слотов/наследования/import-ов даёт solc; downstream не меняется
        auto solc = driver::runSolcMulti(files, solcVersion, remappings);
        if (!solc.ok) {
            std::cerr << "error: solc: " << solc.error << "\n";
            emitSkip(classifySolc(solc.error), "solc " + solcVersion);
            return 1;
        }
        mods = lowering.lowerAll(solc.root, primaryUnit, files, targetContract);
        if (diag.hasErrors()) { printDiagnostics(diag); emitSkip("B1", "lowering-fail"); return 1; }
        if (mods.empty()) {
            std::cerr << "error: solc-фронтенд не дал контракта"
                      << (targetContract.empty() ? "" : " '" + targetContract + "'") << "\n";
            emitSkip("B1", "no-contract");
            return 1;
        }
    }

    if (opts.emitIR) {
        std::string ir;
        llvm::raw_string_ostream os(ir);
        for (auto& mod : mods) mod->print(os, /*AAW=*/nullptr);
        os.flush();
        if (!opts.outputPath.empty()) {
            std::ofstream file(opts.outputPath);
            if (!file.is_open()) {
                std::cerr << "error: cannot open output '" << opts.outputPath << "'\n";
                return 1;
            }
            file << ir;
        } else {
            std::cout << ir;
        }
    }

    // --emit-klee: понижаем модуль с уязвимой функцией в KLEE-исполнимый IR (K1).
    // точку входа берём из явного --klee-entry или из первой HIGH reentrancy-находки
    if (opts.emitKlee) {
        const bool acMode = (opts.kleeMode == "access-control");
        for (auto& mod : mods) {
            std::string entry = opts.kleeEntry;
            int effectSlot = opts.kleeSlot;       // reentrancy: слот-эффект; access-control: privSlot
            int ownerSlot  = opts.kleeOwnerSlot;  // access-control: слот owner
            if (entry.empty()) {
                // копию анализируем (promoteLocals мутирует IR) — не портим исходный модуль
                auto probe = llvm::CloneModule(*mod);
                analyzer::promoteLocals(*probe);
                if (acMode) {
                    analyzer::AccessControlDetector acDet;
                    for (const auto& f : acDet.analyze(*probe))
                        if (f.severity == "HIGH" && f.rule == "access-control" && f.storageSlot >= 0) {
                            entry = f.function; effectSlot = f.storageSlot; ownerSlot = f.ownerSlot; break;
                        }
                } else {
                    analyzer::ReentrancyDetector rDet;
                    for (const auto& f : rDet.analyze(*probe))
                        if (f.severity == "HIGH" && f.rule == "reentrancy") {
                            entry = f.function; effectSlot = f.storageSlot; break;
                        }
                }
            }
            if (entry.empty()) continue;   // в этом контракте нечего подтверждать
            klee::KleeLowering low;
            auto km = low.lower(*mod, entry, effectSlot,
                                acMode ? klee::Mode::AccessControl : klee::Mode::Reentrancy,
                                ownerSlot);
            if (!km) { std::cerr << "error: klee lowering failed for entry '" << entry << "'\n"; return 1; }
            std::string ir;
            llvm::raw_string_ostream os(ir);
            km->print(os, nullptr);
            os.flush();
            if (!opts.outputPath.empty()) {
                std::ofstream file(opts.outputPath);
                if (!file.is_open()) { std::cerr << "error: cannot open output '" << opts.outputPath << "'\n"; return 1; }
                file << ir;
            } else {
                std::cout << ir;
            }
        }
    }

    if (opts.audit) {
        report::Findings findings;
        std::optional<report::Finding> pocFinding;   // первая HIGH reentrancy для PoC
        std::string pocContract;
        for (auto& mod : mods) {
            auto f = runDetectors(*mod);
            // для PoC берём первую HIGH reentrancy; если её нет — HIGH access-control
            if (!pocFinding)
                for (const auto& x : f)
                    if (x.severity == "HIGH" &&
                        (x.rule == "reentrancy" || x.rule == "access-control")) {
                        pocFinding = x;
                        pocContract = mod->getName().str();
                        if (x.rule == "reentrancy") break;   // reentrancy приоритетнее
                    }
            findings.insert(findings.end(), f.begin(), f.end());
        }
        // бейдж [SYM-CONFIRMED]: помечаем находки функций из --confirmed (ров виден в отчёте)
        if (!opts.confirmedFns.empty()) {
            std::vector<std::string> conf;
            for (size_t p = 0; p < opts.confirmedFns.size();) {
                size_t c = opts.confirmedFns.find(',', p);
                if (c == std::string::npos) c = opts.confirmedFns.size();
                conf.push_back(opts.confirmedFns.substr(p, c - p));
                p = c + 1;
            }
            // KLEE подтверждает только reentrancy/access-control — только их и метим
            // (иначе unchecked-call/tx-origin на той же функции получили бы ложный бейдж)
            for (auto& f : findings)
                if (f.rule == "reentrancy" || f.rule == "access-control")
                    for (const auto& name : conf)
                        if (f.function == name) { f.symConfirmed = true; break; }
        }

        // консервативный дедуп: одинаковые (rule, function, line, storageSlot) — оставляем одну
        {
            report::Findings uniq;
            for (auto& f : findings) {
                bool dup = false;
                for (const auto& u : uniq)
                    if (u.rule == f.rule && u.function == f.function &&
                        u.line == f.line && u.storageSlot == f.storageSlot) { dup = true; break; }
                if (!dup) uniq.push_back(f);
            }
            findings = std::move(uniq);
        }
        // триаж: считаем приоритет и сортируем по убыванию (важное — первым)
        for (auto& f : findings) f.priorityScore = scoreFinding(f);
        std::stable_sort(findings.begin(), findings.end(),
                         [](const report::Finding& a, const report::Finding& b) {
                             return a.priorityScore > b.priorityScore;
                         });

        // порог severity (--min-severity): отбрасываем находки ниже порога
        if (!opts.minSeverity.empty()) {
            int minR = sevRank(opts.minSeverity);
            report::Findings kept;
            for (auto& f : findings) if (sevRank(f.severity) >= minR) kept.push_back(f);
            findings = std::move(kept);
        }
        if (opts.llmContext) {
            // нейро-символический контекст-пак: факты IR + зацепки + гипотезы + исходник.
            // пишем в --output если задан, иначе в stdout (для перехвата Claude Code)
            report::ContextPackWriter w;
            report::ContextMeta meta;
            meta.source      = opts.address.empty() ? opts.inputPath : opts.address;
            meta.solcVersion = solcVersion;
            meta.address     = opts.address;
            meta.chain       = opts.chain;
            if (!opts.outputPath.empty()) {
                std::ofstream f(opts.outputPath);
                if (!f.is_open()) {
                    std::cerr << "error: cannot open output '" << opts.outputPath << "'\n";
                    return 1;
                }
                w.write(mods, findings, files, meta, f);
                std::cerr << "context-pack записан: " << opts.outputPath << "\n";
            } else {
                w.write(mods, findings, files, meta, std::cout);
            }
        } else if (opts.sarif) {
            report::SarifReporter reporter;
            reporter.report(findings, opts.inputPath, std::cout);
        } else if (opts.jsonOutput) {
            report::JsonReporter reporter;
            reporter.report(findings, std::cout);
        } else {
            report::ConsoleReporter reporter;
            reporter.report(findings, std::cout);
        }
        // PoC (ров, AD-7): по первой HIGH reentrancy среди всех контрактов
        if (opts.genPoc) {
            if (pocFinding) {
                report::PocGenerator pg;
                report::KleeWitness witness;
                witness.confirmed = opts.kleeConfirmed;
                witness.balance   = opts.kleeBalance;
                witness.msgValue  = opts.kleeMsgValue;
                // разбираем "v0,v1,…" в вектор конкретных аргументов
                for (size_t p = 0; p < opts.kleeArgs.size();) {
                    size_t c = opts.kleeArgs.find(',', p);
                    if (c == std::string::npos) c = opts.kleeArgs.size();
                    witness.argValues.push_back(opts.kleeArgs.substr(p, c - p));
                    p = c + 1;
                }
                report::ForkConfig fork;
                fork.enabled   = !opts.pocVictim.empty();   // --victim включает форк-режим
                fork.victim    = opts.pocVictim;
                fork.forkBlock = opts.pocForkBlock;
                fork.rpcEnv    = opts.pocRpcEnv;
                const bool isAC = (pocFinding->rule == "access-control");
                std::string poc = isAC
                    ? pg.generateAccessControl(pocContract, *pocFinding, witness, fork)
                    : pg.generateReentrancy(pocContract, *pocFinding, witness, fork);
                std::string path = pocContract + (isAC ? "AccessControlPoC.t.sol"
                                                       : "ReentrancyPoC.t.sol");
                std::ofstream pf(path);
                if (pf.is_open()) { pf << poc; std::cerr << "PoC записан: " << path << "\n"; }
                else std::cout << poc;
            } else {
                std::cerr << "--poc: нет HIGH reentrancy/access-control находки для генерации PoC\n";
            }
        }
        // инвариант-харнесс (Фаза B): свойство выбирается --invariant-property
        if (opts.emitInvariant) {
            report::InvariantGen ig;
            const std::string& prop = opts.invariantProperty;
            if (prop == "inflation" || prop == "inflation-erc20" || prop == "inflation-4626" ||
                prop == "roundtrip" || prop == "supply") {
                // share-inflation / round-trip округления: находка не нужна, форма по конвенции.
                // цель = первый контракт модуля
                std::string cname = mods.empty() ? "" : mods.front()->getName().str();
                if (!cname.empty()) {
                    std::string harness, path;
                    if (prop == "inflation-erc20") {
                        harness = ig.generateInflationErc20(cname);
                        path = cname + "Erc20InflationInvariant.t.sol";
                    } else if (prop == "inflation-4626") {
                        harness = ig.generateInflationErc4626(cname);
                        path = cname + "Erc4626InflationInvariant.t.sol";
                    } else if (prop == "roundtrip") {
                        // --victim → форк-режим: инвариант против РЕАЛЬНОГО ERC4626 по адресу
                        report::ForkConfig fork;
                        fork.enabled   = !opts.pocVictim.empty();
                        fork.victim    = opts.pocVictim;
                        fork.forkBlock = opts.pocForkBlock;
                        fork.rpcEnv    = opts.pocRpcEnv;
                        harness = ig.generateRoundtrip(cname, fork);
                        path = cname + (fork.enabled ? "RoundtripForkInvariant.t.sol"
                                                     : "RoundtripInvariant.t.sol");
                    } else if (prop == "supply") {
                        harness = ig.generateSupply(cname);
                        path = cname + "SupplyInvariant.t.sol";
                    } else {
                        harness = ig.generateInflation(cname);
                        path = cname + "InflationInvariant.t.sol";
                    }
                    std::ofstream hf(path);
                    if (hf.is_open()) { hf << harness; std::cerr << "инвариант-харнесс записан: " << path << "\n"; }
                    else std::cout << harness;
                } else {
                    std::cerr << "--emit-invariant " << prop << ": нет контракта\n";
                }
            } else if (pocFinding && pocFinding->rule == "reentrancy") {
                // solvency/no-overdraw: по первой HIGH reentrancy (vault-форма депозит/вывод)
                std::string harness = ig.generateSolvency(pocContract, *pocFinding);
                std::string path = pocContract + "SolvencyInvariant.t.sol";
                std::ofstream hf(path);
                if (hf.is_open()) { hf << harness; std::cerr << "инвариант-харнесс записан: " << path << "\n"; }
                else std::cout << harness;
            } else {
                std::cerr << "--emit-invariant: нет HIGH reentrancy-находки для solvency-харнесса "
                             "(для share-inflation укажи --invariant-property inflation)\n";
            }
        }

        if (!findings.empty()) return 2;
    }

    return 0;
}

}  // namespace solc
