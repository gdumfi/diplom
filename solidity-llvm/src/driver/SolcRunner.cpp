#include "solc/driver/SolcRunner.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#include <llvm/Support/raw_ostream.h>

namespace solc::driver {
namespace {

// прогнать shell-команду, вернуть stdout целиком
std::string runCmd(const std::string& cmd) {
    std::string out;
    std::array<char, 65536> buf;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return out;
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), p)) > 0) out.append(buf.data(), n);
    pclose(p);
    return out;
}

// сериализовать standard-json input для НЕСКОЛЬКИХ файлов. llvm::json сам эскейпит контент.
std::string buildInputMulti(const std::vector<std::pair<std::string, std::string>>& files,
                            const std::vector<std::string>& remappings) {
    using namespace llvm;
    json::Object sources;
    for (const auto& [path, content] : files) {
        json::Object src;
        src["content"] = content;
        sources[path] = std::move(src);
    }

    // outputSelection: {"*": {"*": ["storageLayout"], "": ["ast"]}}
    json::Object perContract;
    perContract["*"] = json::Array{"storageLayout", "evm.methodIdentifiers"};
    perContract[""] = json::Array{"ast"};
    json::Object outSel;
    outSel["*"] = std::move(perContract);
    json::Object settings;
    settings["outputSelection"] = std::move(outSel);
    // remappings (@openzeppelin/=... solmate/=...) — иначе solc не резолвит import-ы
    if (!remappings.empty()) {
        json::Array rm;
        for (const auto& r : remappings) rm.push_back(r);
        settings["remappings"] = std::move(rm);
    }

    json::Object input;
    input["language"] = "Solidity";
    input["sources"] = std::move(sources);
    input["settings"] = std::move(settings);

    std::string s;
    raw_string_ostream os(s);
    os << json::Value(std::move(input));
    return os.str();
}

// достать первую ошибку компиляции (severity=="error") из d["errors"], либо пусто
std::string firstError(const llvm::json::Object* d) {
    const auto* errs = d->getArray("errors");
    if (!errs) return "";
    for (const auto& e : *errs) {
        const auto* eo = e.getAsObject();
        if (!eo) continue;
        auto sev = eo->getString("severity");
        if (sev && *sev == "error") {
            auto msg = eo->getString("formattedMessage");
            if (!msg) msg = eo->getString("message");
            return msg ? msg->str() : "solc compile error";
        }
    }
    return "";
}

}  // namespace

namespace {
// прогнать готовый standard-json input через docker solc, распарсить вывод
SolcResult runSolcInput(const std::string& input, const std::string& solcVersion) {
    SolcResult r;
    char tmpl[] = "/tmp/scsa_solc_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) { r.error = "не удалось создать временный файл для solc"; return r; }
    if (write(fd, input.data(), input.size()) != static_cast<ssize_t>(input.size())) {
        close(fd); std::remove(tmpl);
        r.error = "не удалось записать input.json"; return r;
    }
    close(fd);
    // ethereum/solc: entrypoint = solc, версия пинится тегом. content встроен → без монтирования
    std::string cmd = "docker run --rm -i ethereum/solc:" + solcVersion +
                      " --standard-json < " + tmpl + " 2>/dev/null";
    std::string out = runCmd(cmd);
    std::remove(tmpl);
    if (out.empty()) {
        r.error = "solc не дал вывода (нет docker/образа ethereum/solc:" + solcVersion + "?)";
        return r;
    }
    auto parsed = llvm::json::parse(out);
    if (!parsed) { llvm::consumeError(parsed.takeError());
        r.error = "не удалось разобрать вывод solc как JSON"; return r; }
    const auto* d = parsed->getAsObject();
    if (!d) { r.error = "вывод solc — не JSON-объект"; return r; }
    std::string ce = firstError(d);
    if (!ce.empty()) { r.error = ce; return r; }
    r.root = std::move(*parsed);
    r.ok = true;
    return r;
}
}  // namespace

SolcResult runSolc(const std::string& source, const std::string& unitName,
                   const std::string& solcVersion) {
    return runSolcInput(buildInputMulti({{unitName, source}}, {}), solcVersion);
}

SolcResult runSolcMulti(const std::vector<std::pair<std::string, std::string>>& sources,
                        const std::string& solcVersion,
                        const std::vector<std::string>& remappings) {
    if (sources.empty()) { SolcResult r; r.error = "solc: пустой список файлов"; return r; }
    return runSolcInput(buildInputMulti(sources, remappings), solcVersion);
}

void buildIdIndex(const llvm::json::Value& node,
                  std::unordered_map<int64_t, const llvm::json::Object*>& index) {
    if (const auto* o = node.getAsObject()) {
        if (auto id = o->getInteger("id")) index[*id] = o;
        for (const auto& kv : *o) buildIdIndex(kv.second, index);
    } else if (const auto* a = node.getAsArray()) {
        for (const auto& e : *a) buildIdIndex(e, index);
    }
}

}  // namespace solc::driver
