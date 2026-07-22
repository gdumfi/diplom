#include <cxxopts.hpp>
#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>
#include <unistd.h>

#include "solc/driver/Driver.h"

int main(int argc, char** argv) {
    // подкоманда `sca tui` — запускаем терминальный интерфейс (отдельный бинарь scsa-tui).
    // себя передаём как SCA_BIN, чтобы TUI звал этот же анализатор.
    if (argc >= 2 && std::string(argv[1]) == "tui") {
        char self[4096];
        ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n > 0) { self[n] = '\0'; setenv("SCA_BIN", self, 0); }  // не перезаписываем заданный
        const char* tuiBin = std::getenv("SCSA_TUI");
        std::vector<char*> av;
        av.push_back(const_cast<char*>(tuiBin && *tuiBin ? tuiBin : "scsa-tui"));
        for (int i = 2; i < argc; ++i) av.push_back(argv[i]);
        av.push_back(nullptr);
        execvp(av[0], av.data());
        std::cerr << "не удалось запустить TUI: положи scsa-tui в PATH или задай SCSA_TUI\n";
        return 1;
    }

    cxxopts::Options opts("solidity-llvm",
                          "Solidity subset -> LLVM IR compiler (MVP)");
    opts.add_options()
        ("i,input",    "Input .sol file", cxxopts::value<std::string>())
        ("address",    "Fetch verified source by address (Etherscan V2 by default; any Blockscout/Orbit explorer via --explorer-url)", cxxopts::value<std::string>()->default_value(""))
        ("chain",      "Chain id for --address (1=mainnet, 56=BSC, 8453=Base, ...)", cxxopts::value<int>()->default_value("1"))
        ("explorer-url", "Custom Blockscout/Orbit explorer API base for --address (e.g. https://<host>/api) — reaches L2/L3 chains without Etherscan API", cxxopts::value<std::string>()->default_value(""))
        ("o,output",   "Output file (default stdout)",
             cxxopts::value<std::string>()->default_value(""))
        ("emit-ir",    "Emit LLVM IR")
        ("legacy",     "Use the legacy hand-rolled frontend instead of solc (0.4.x subset)")
        ("solc-version", "solc image tag for the solc frontend (default 0.8.19)", cxxopts::value<std::string>()->default_value("0.8.19"))
        ("audit",      "Run security analyzer (reentrancy) and print findings")
        ("json",       "Print audit findings as JSON")
        ("poc",        "Generate a Foundry reentrancy PoC for the first HIGH finding")
        ("emit-invariant", "Generate a Foundry stateful-invariant harness for the target")
        ("invariant-property", "Property for --emit-invariant: solvency|inflation|inflation-erc20|roundtrip|supply", cxxopts::value<std::string>()->default_value("solvency"))
        ("sarif",      "Output findings in SARIF 2.1.0 (for CI/GitHub)")
        ("llm-context", "Emit a neuro-symbolic context pack (IR facts + leads + hypotheses + source) for Claude Code analysis")
        ("min-severity", "Only report findings at or above LEVEL (HIGH|MEDIUM|LOW)", cxxopts::value<std::string>()->default_value(""))
        ("confirmed", "Mark findings of these functions as [SYM-CONFIRMED] (comma-separated)", cxxopts::value<std::string>()->default_value(""))
        ("emit-klee",  "Lower flagged reentrancy into a KLEE-executable LLVM module")
        ("klee-entry", "Entry function for --emit-klee harness (default: auto from finding)", cxxopts::value<std::string>()->default_value(""))
        ("klee-mode", "Vuln class for --emit-klee: reentrancy|access-control", cxxopts::value<std::string>()->default_value("reentrancy"))
        ("klee-slot", "Effect/privileged slot for manual --klee-entry", cxxopts::value<int>()->default_value("-1"))
        ("klee-owner-slot", "Owner slot for manual --klee-entry (access-control)", cxxopts::value<int>()->default_value("-1"))
        ("klee-confirmed", "Mark generated PoC (--poc) as symbolically confirmed by KLEE")
        ("klee-balance", "KLEE witness: balance (storage0) for the confirmed PoC", cxxopts::value<std::string>()->default_value(""))
        ("klee-msg-value", "KLEE witness: msg.value for the confirmed PoC", cxxopts::value<std::string>()->default_value(""))
        ("klee-args", "KLEE witness: concrete args of the vulnerable fn, comma-separated", cxxopts::value<std::string>()->default_value(""))
        ("victim", "PoC fork mode: deployed victim address (0x...) — targets a real contract", cxxopts::value<std::string>()->default_value(""))
        ("fork-block", "PoC fork mode: fork block number (before the patch)", cxxopts::value<std::string>()->default_value("0"))
        ("fork-rpc-env", "PoC fork mode: env var holding the RPC URL", cxxopts::value<std::string>()->default_value("RPC_URL"))
        ("h,help",     "Print help");

    cxxopts::ParseResult result;
    try {
        result = opts.parse(argc, argv);
    } catch (const cxxopts::exceptions::exception& e) {
        std::cerr << "error: " << e.what() << "\n\n";
        std::cerr << opts.help() << '\n';
        return 1;
    }

    // нужен либо --input, либо --address (Etherscan-фетч)
    if (result.count("help") || (!result.count("input") && !result.count("address"))) {
        std::cout << opts.help() << '\n';
        return result.count("help") ? 0 : 1;
    }

    solc::CompileOptions co;
    co.inputPath  = result.count("input") ? result["input"].as<std::string>() : "";
    co.address    = result["address"].as<std::string>();
    co.chain      = result["chain"].as<int>();
    // --explorer-url пробрасываем в env: фетч-слой читает SCSA_EXPLORER_URL (без правки сигнатур)
    if (auto ex = result["explorer-url"].as<std::string>(); !ex.empty())
        setenv("SCSA_EXPLORER_URL", ex.c_str(), 1);
    co.outputPath = result["output"].as<std::string>();
    co.emitIR     = result.count("emit-ir") > 0;
    co.legacy     = result.count("legacy") > 0;
    co.solcVersion = result["solc-version"].as<std::string>();
    co.jsonOutput = result.count("json") > 0;
    co.genPoc     = result.count("poc") > 0;
    co.emitInvariant = result.count("emit-invariant") > 0;
    co.invariantProperty = result["invariant-property"].as<std::string>();
    co.sarif      = result.count("sarif") > 0;
    co.llmContext = result.count("llm-context") > 0;
    co.minSeverity = result["min-severity"].as<std::string>();
    co.confirmedFns = result["confirmed"].as<std::string>();
    co.emitKlee   = result.count("emit-klee") > 0;
    co.kleeEntry  = result["klee-entry"].as<std::string>();
    co.kleeMode   = result["klee-mode"].as<std::string>();
    co.kleeSlot   = result["klee-slot"].as<int>();
    co.kleeOwnerSlot = result["klee-owner-slot"].as<int>();
    co.kleeConfirmed = result.count("klee-confirmed") > 0;
    co.kleeBalance   = result["klee-balance"].as<std::string>();
    co.kleeMsgValue  = result["klee-msg-value"].as<std::string>();
    co.kleeArgs      = result["klee-args"].as<std::string>();
    co.pocVictim     = result["victim"].as<std::string>();
    co.pocForkBlock  = result["fork-block"].as<std::string>();
    co.pocRpcEnv     = result["fork-rpc-env"].as<std::string>();
    // --poc/--sarif/--llm-context/--emit-invariant требуют анализа, поэтому включают audit неявно
    co.audit      = result.count("audit") > 0 || co.genPoc || co.sarif || co.llmContext
                    || co.emitInvariant;

    solc::Driver drv;
    return drv.run(co);
}
