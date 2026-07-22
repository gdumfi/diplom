#pragma once
#include <string>

namespace solc {

struct CompileOptions {
    std::string inputPath;
    std::string outputPath;
    // тянуть верифицированный исходник по адресу с Etherscan вместо чтения файла
    std::string address;   // 0x... (--address); пусто = читать inputPath
    int         chain = 1; // chainId (--chain); 1 = Ethereum mainnet
    // фронтенд: по умолчанию solc --standard-json + AstJsonLowering (боевой Solidity 0.8+).
    // --legacy включает старый рукописный lexer/parser/sema (только для регресса/сравнения)
    bool        legacy = false;
    std::string solcVersion = "0.8.19";  // тег образа ethereum/solc для solc-фронтенда
    bool emitIR = false;
    bool audit  = false;   // прогнать анализатор безопасности и напечатать находки
    bool jsonOutput = false;   // выводить находки в JSON (--json), иначе консольный формат
    bool sarif = false;        // выводить находки в SARIF 2.1.0 (--sarif) для CI/GitHub
    // собрать нейро-символический контекст-пак (--llm-context) для анализа Claude Code:
    // факты из IR (слоты/вызовы/гварды) + зацепки детекторов + гипотезы-хуки + исходник.
    // «сенсор для LLM-аналитика» — Фаза A нейро-символической схемы (см. HISTORY)
    bool llmContext = false;
    bool genPoc = false;       // сгенерировать Foundry-PoC по найденной reentrancy (--poc)
    // сгенерировать Foundry stateful-invariant харнесс (--emit-invariant) по vault-находке:
    // свойство no-overdraw (withdrawn<=deposited) — фаззер ломает его на reentrancy/битом учёте
    bool emitInvariant = false;
    // какое свойство генерить (--invariant-property): solvency (no-overdraw, по reentrancy-находке)
    // | inflation (first-depositor/share-inflation, по форме deposit/redeem — находка не нужна)
    std::string invariantProperty = "solvency";
    std::string minSeverity;   // порог severity (--min-severity HIGH|MEDIUM|LOW); пусто = все
    // функции, символьно подтверждённые KLEE (--confirmed "fn1,fn2") — их находки
    // помечаются [SYM-CONFIRMED] в отчёте. заполняется run_klee.sh после прогона KLEE
    std::string confirmedFns;
    // понизить IR в KLEE-исполнимый модуль для символьного подтверждения (--emit-klee).
    // харнесс входит в функцию из первой HIGH reentrancy-находки (или entryFn ниже)
    bool emitKlee = false;
    std::string kleeEntry;     // имя уязвимой функции для харнесса (пусто = авто из находки)
    std::string kleeMode = "reentrancy";  // класс для --emit-klee: reentrancy|access-control
    int kleeSlot = -1;        // effectSlot/privSlot при ручном --klee-entry (иначе из находки)
    int kleeOwnerSlot = -1;   // ownerSlot при ручном --klee-entry (access-control)
    // witness-модель KLEE для PoC (--poc): заполняется run_klee.sh после символьного
    // подтверждения. kleeConfirmed=true → PoC помечается «подтверждён», TODO снимаются
    bool kleeConfirmed = false;
    std::string kleeBalance;   // storage0 из ktest (баланс-свидетель, десятичное)
    std::string kleeMsgValue;  // msg_value из ktest
    std::string kleeArgs;      // конкретные аргументы уязвимой функции из ktest, "v0,v1,…"
    // форк-режим PoC: эксплойт против развёрнутой жертвы (--victim включает форк-режим)
    std::string pocVictim;         // адрес развёрнутой жертвы (0x...)
    std::string pocForkBlock = "0"; // блок форка (до патча)
    std::string pocRpcEnv = "RPC_URL"; // env-переменная с RPC-URL
};

class Driver {
public:
    int run(const CompileOptions& opts);
};

}  // namespace solc
