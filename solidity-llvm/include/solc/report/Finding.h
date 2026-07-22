#pragma once
#include <string>
#include <vector>

namespace solc::report {

// одна находка анализатора. severity/rule — строки для простоты вывода и JSON.
struct Finding {
    std::string rule;       // идентификатор детектора, напр. "reentrancy"
    std::string severity;   // "HIGH" | "MEDIUM" | "LOW"
    std::string function;   // имя функции, где найдено
    std::string message;    // человекочитаемое описание
    int         line = -1;          // номер исходной строки (-1 если неизвестен)
    // символьно подтверждено KLEE (ров, AD-7): путь к уязвимости реально достижим, не
    // просто паттерн. проставляется по имени функции из --confirmed (см. Driver/run_klee).
    bool        symConfirmed = false;
    // приоритет триажа (больше = важнее): деньги×severity + бонус за KLEE-подтверждение.
    // считается в Driver (scoreFinding), находки сортируются по убыванию перед отчётом
    int         priorityScore = 0;
    // слот storage-записи после внешнего вызова (-1 если неизвестен). для mapping это
    // БАЗОВЫЙ слот переменной, а не адрес элемента — какой именно ключ/пользователь
    // затронут, находка не различает (все элементы одного mapping делят базовый слот)
    int         storageSlot = -1;
    // слот owner (сравнивается с msg.sender) — для access-control находок и KLEE-оракула
    // «атакующий msg.sender != owner достигает привилегированной записи». -1 если неизвестен
    int         ownerSlot = -1;

    // --- контекст для генерации PoC (заполняется детектором) ---
    int         vulnParamCount = 0;      // число параметров уязвимой функции
    std::string fundingFunction;         // функция-депозит (пополняет тот же слот через msg.value); пусто если нет
    int         fundingParamCount = 0;   // число параметров функции-депозита
    // Solidity-типы параметров (address/uint256/bool) — для типизированных заглушек в PoC
    std::vector<std::string> vulnParamTypes;
    std::vector<std::string> fundingParamTypes;
};

using Findings = std::vector<Finding>;

}  // namespace solc::report
