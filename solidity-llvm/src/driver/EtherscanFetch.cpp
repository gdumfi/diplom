#include "solc/driver/EtherscanFetch.h"

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <utility>

#include <llvm/Support/JSON.h>

namespace solc::driver {
namespace {

// адрес: 0x + ровно 40 hex. валидация ДО подстановки в shell-команду (защита от инъекции)
bool validAddress(const std::string& a) {
    if (a.size() != 42 || a[0] != '0' || (a[1] != 'x' && a[1] != 'X')) return false;
    for (size_t i = 2; i < a.size(); ++i)
        if (!std::isxdigit(static_cast<unsigned char>(a[i]))) return false;
    return true;
}

// python-экстрактор: разбирает ответ Etherscan, выдаёт КАЖДЫЙ файл отдельно как
// SCSA_JSON:{"name":..,"files":{path:content,..}}. multi-file standard-json ({{..}})
// раскрывается в отдельные записи, плоский исходник — как один файл name.sol
const char* kExtractPy = R"PY(import json,sys,re
try: d=json.load(sys.stdin)
except: print("SCSA_ERR:invalid response"); sys.exit()
if str(d.get("status"))!="1": print("SCSA_ERR:"+str(d.get("result") or d.get("message"))); sys.exit()
r=d["result"][0]; src=r.get("SourceCode",""); name=r.get("ContractName","Contract")
m=re.search(r"v?(\d+\.\d+\.\d+)", r.get("CompilerVersion","")); ver=m.group(1) if m else "0.8.19"
proxy=str(r.get("Proxy","")); impl=(r.get("Implementation") or "").strip()
pim=impl if (proxy=="1" and re.match(r"^0x[0-9a-fA-F]{40}$", impl or "") and int(impl,16)!=0) else ""
files={}; remaps=[]
if src[:2]=="{{" or src[:1]=="{":
  try:
    j=json.loads(src[1:-1] if src[:2]=="{{" else src)
    srcs=j.get("sources", j)
    for path,v in srcs.items():
      files[path]= v.get("content","") if isinstance(v,dict) else str(v)
    remaps=j.get("settings",{}).get("remappings",[]) or []
  except: files={name+".sol": src}
else:
  files={name+".sol": src}
# Blockscout (Etherscan-совместимый) отдаёт доп. файлы отдельным списком AdditionalSources
add=r.get("AdditionalSources")
if isinstance(add,list):
  for a in add:
    fn=(a.get("Filename") or a.get("FileName") or "").strip()
    sc=a.get("SourceCode","")
    if fn and sc: files[fn]=sc
if not files or not any(files.values()): print("SCSA_ERR:empty source (не верифицирован?)"); sys.exit()
print("SCSA_JSON:"+json.dumps({"name":name,"version":ver,"files":files,"remappings":remaps,"proxyImpl":pim}))
)PY";

// экстрактор ответа Sourcify API v2 (/v2/contract/{chain}/{addr}?fields=sources,compilation).
// v1 отключён («API v1 Brownout», 2025) → используем v2: sources = {path:{content}},
// compilation = {compilerVersion, name}. без ключа, десятки сетей → фолбэк, когда explorer молчит.
const char* kSourcifyPy = R"PY(import json,sys,re
try: d=json.load(sys.stdin)
except: print("SCSA_ERR:invalid sourcify response"); sys.exit()
if not isinstance(d,dict) or not d.get("sources"):
  print("SCSA_ERR:"+str((d or {}).get("error") or (d or {}).get("message") or "not on sourcify")); sys.exit()
files={}
for path,v in (d.get("sources") or {}).items():
  c=v.get("content","") if isinstance(v,dict) else str(v)
  if c: files[path]=c
comp=d.get("compilation") or {}
m=re.search(r"(\d+\.\d+\.\d+)", comp.get("compilerVersion","") or ""); ver=m.group(1) if m else "0.8.19"
name=comp.get("name") or "Contract"
if not files: print("SCSA_ERR:no sources in sourcify v2"); sys.exit()
print("SCSA_JSON:"+json.dumps({"name":name,"version":ver,"files":files,"remappings":[],"proxyImpl":""}))
)PY";

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

// какой explorer дёргать. blockscout=true → Blockscout/Orbit-совместимый инстанс (L2/L3,
// напр. Intuition): тот же getsourcecode-формат, но привязан к одной сети (без chainid),
// обычно без apikey. blockscout=false → единый Etherscan V2 API (параметр chainid).
struct Explorer {
    std::string apiBase;   // базовый API-путь (напр. https://<host>/api)
    bool blockscout;
};

// приоритет: явный env/флаг SCSA_EXPLORER_URL (любой Blockscout-инстанс) → дефолт Etherscan V2.
// расширение охвата за пределы Etherscan-совместимых сетей: хантер указывает --explorer-url
// на кастомный explorer (Orbit/L3), и --address начинает работать там, где Etherscan-API нет.
Explorer resolveExplorer() {
    if (const char* e = std::getenv("SCSA_EXPLORER_URL"); e && *e) {
        std::string base(e);
        // удобство: если дали корень хоста без /api — допишем
        if (base.size() >= 4 && base.substr(base.size() - 4) != "/api"
            && base.find("/api?") == std::string::npos)
            base += "/api";
        return { base, true };
    }
    return { "https://api.etherscan.io/v2/api", false };
}

// разбор вывода экстрактора (SCSA_JSON/SCSA_ERR) в поля. общий для Etherscan/Blockscout/Sourcify
bool parseExtractorOut(const std::string& out, std::string& name, std::string& ver,
                       std::vector<std::pair<std::string, std::string>>& files,
                       std::vector<std::string>& remaps, std::string& proxyImpl, std::string& err) {
    auto errPos = out.find("SCSA_ERR:");
    if (errPos != std::string::npos) {
        err = out.substr(errPos + 9);
        if (auto nl = err.find('\n'); nl != std::string::npos) err = err.substr(0, nl);
        if (err.find("Missing/Invalid API Key") != std::string::npos)
            err += " — задай ETHERSCAN_API_KEY";
        return false;
    }
    auto jpos = out.find("SCSA_JSON:");
    if (jpos == std::string::npos) {
        err = "не удалось получить исходник (нет curl/python3? нет сети?)";
        return false;
    }
    std::string jsonStr = out.substr(jpos + 10);
    auto parsed = llvm::json::parse(jsonStr);
    if (!parsed) {
        llvm::consumeError(parsed.takeError());
        err = "не удалось разобрать ответ экстрактора";
        return false;
    }
    const auto* o = parsed->getAsObject();
    if (!o) { err = "ответ экстрактора — не объект"; return false; }
    if (auto n = o->getString("name")) name = n->str();
    if (auto v = o->getString("version")) ver = v->str();
    files.clear();
    if (const auto* fo = o->getObject("files"))
        for (const auto& kv : *fo)
            if (auto c = kv.second.getAsString())
                files.emplace_back(kv.first.str(), c->str());
    remaps.clear();
    if (const auto* ra = o->getArray("remappings"))
        for (const auto& e : *ra)
            if (auto s = e.getAsString()) remaps.push_back(s->str());
    proxyImpl.clear();
    if (auto pim = o->getString("proxyImpl")) proxyImpl = pim->str();
    if (files.empty()) { err = "пустой исходник (контракт не верифицирован?)"; return false; }
    return true;
}

}  // namespace

// один запрос getsourcecode + разбор. proxyImpl != "" если Etherscan пометил контракт прокси
// (EIP-1967) и вернул адрес имплементации — вызывающий перенаправляется на неё (пронзание прокси)
static bool fetchOnce(const std::string& address, int chainId, std::string& contractName,
                      std::string& compilerVersion,
                      std::vector<std::pair<std::string, std::string>>& files,
                      std::vector<std::string>& remappings,
                      std::string& proxyImpl, std::string& err) {
    if (!validAddress(address)) {
        err = "некорректный адрес (ожидается 0x + 40 hex)";
        return false;
    }
    const char* key = std::getenv("ETHERSCAN_API_KEY");
    std::string apiKey = key ? key : "";

    Explorer ex = resolveExplorer();
    std::ostringstream url;
    if (ex.blockscout) {
        // Blockscout/Orbit: инстанс привязан к сети (chainId не нужен), ключ обычно не требуется
        url << ex.apiBase << "?module=contract&action=getsourcecode&address=" << address;
        if (!apiKey.empty()) url << "&apikey=" << apiKey;  // некоторые инстансы принимают ключ
    } else {
        url << ex.apiBase << "?chainid=" << chainId
            << "&module=contract&action=getsourcecode&address=" << address
            << "&apikey=" << apiKey;
    }

    // curl → python3 (python двойными кавычками, shell-обёртка одинарными → без конфликта)
    std::ostringstream cmd;
    cmd << "curl -s --max-time 25 '" << url.str() << "' | python3 -c '" << kExtractPy << "'";
    std::string out = runCmd(cmd.str());
    return parseExtractorOut(out, contractName, compilerVersion, files, remappings, proxyImpl, err);
}

// фолбэк: Sourcify (без API-ключа, десятки сетей). полезно, когда explorer не отдал исходник
// (не верифицирован на explorer, но верифицирован на Sourcify — частый случай для L2/L3/форджа)
static bool fetchFromSourcify(const std::string& address, int chainId, std::string& contractName,
                              std::string& compilerVersion,
                              std::vector<std::pair<std::string, std::string>>& files,
                              std::vector<std::string>& remappings, std::string& err) {
    if (!validAddress(address)) { err = "некорректный адрес"; return false; }
    std::ostringstream url;
    url << "https://sourcify.dev/server/v2/contract/" << chainId << "/" << address
        << "?fields=sources,compilation";
    // фолбэк тянет полный мультифайл-бандл (может быть крупным) — таймаут щедрее explorer-а
    std::ostringstream cmd;
    cmd << "curl -s --max-time 45 '" << url.str() << "' | python3 -c '" << kSourcifyPy << "'";
    std::string out = runCmd(cmd.str());
    std::string proxyImpl;  // Sourcify не резолвит прокси
    return parseExtractorOut(out, contractName, compilerVersion, files, remappings, proxyImpl, err);
}

bool fetchVerifiedSources(const std::string& address, int chainId, std::string& contractName,
                          std::string& compilerVersion,
                          std::vector<std::pair<std::string, std::string>>& files,
                          std::vector<std::string>& remappings,
                          std::string& err) {
    std::string proxyImpl;
    if (!fetchOnce(address, chainId, contractName, compilerVersion, files, remappings, proxyImpl, err)) {
        // фолбэк на Sourcify (без ключа, десятки сетей) — контракт мог быть верифицирован там
        std::string sName, sVer, sErr;
        std::vector<std::pair<std::string, std::string>> sFiles;
        std::vector<std::string> sRemaps;
        if (fetchFromSourcify(address, chainId, sName, sVer, sFiles, sRemaps, sErr) && !sFiles.empty()) {
            std::fprintf(stderr, "explorer не дал исходник (%s); получено с Sourcify: %s (solc %s)\n",
                         err.c_str(), sName.c_str(), sVer.c_str());
            contractName = sName; compilerVersion = sVer;
            files = std::move(sFiles); remappings = std::move(sRemaps);
            return true;  // прокси-пронзание недоступно на Sourcify-пути (нет impl-адреса)
        }
        return false;  // ни explorer, ни Sourcify не дали исходник
    }
    // пронзание прокси: Etherscan пометил контракт прокси (EIP-1967/Transparent) и знает адрес
    // имплементации. хантеру нужна ЛОГИКА (там деньги/баги), а не обёртка — перенаправляемся на
    // имплементацию (1 хоп; если её исходник не достать — честно анализируем обёртку)
    if (!proxyImpl.empty() && validAddress(proxyImpl)) {
        std::string iName, iVer, iErr, iProxy;
        std::vector<std::pair<std::string, std::string>> iFiles;
        std::vector<std::string> iRemaps;
        if (fetchOnce(proxyImpl, chainId, iName, iVer, iFiles, iRemaps, iProxy, iErr) && !iFiles.empty()) {
            std::fprintf(stderr,
                "контракт %s — прокси (EIP-1967); анализируем имплементацию %s (%s)\n",
                contractName.c_str(), iName.c_str(), proxyImpl.c_str());
            contractName = iName; compilerVersion = iVer;
            files = std::move(iFiles); remappings = std::move(iRemaps);
        } else {
            std::fprintf(stderr,
                "контракт — прокси, но имплементацию %s получить не удалось (%s); анализируем обёртку\n",
                proxyImpl.c_str(), iErr.c_str());
        }
    }
    return true;
}

}  // namespace solc::driver
