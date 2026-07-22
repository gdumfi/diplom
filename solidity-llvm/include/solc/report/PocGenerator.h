#pragma once
#include <string>

#include "solc/report/Finding.h"

namespace solc::report {

// witness-модель от KLEE (символьное подтверждение, K2): конкретные входы, при
// которых путь кражи ДОСТИЖИМ. заполняется run_klee.sh из ktest-модели KLEE.
// confirmed=false — KLEE не запускался/не подтвердил, PoC остаётся best-effort-каркасом
struct KleeWitness {
    bool        confirmed = false;
    std::string balance;    // storage0 из ktest: минимальный баланс-свидетель (десятичное)
    std::string msgValue;   // msg_value из ktest
    int         depth = 1;  // глубина реентрантного callback (§6.2)
    // конкретные значения аргументов уязвимой функции из ktest (arg0, arg1, …).
    // если непусто — генератор подставляет их вместо типизированных заглушек
    std::vector<std::string> argValues;
};

// конфигурация форк-режима PoC: эксплойт против РЕАЛЬНО развёрнутой жертвы.
// вместо `new Contract()` — форк сети на блоке ДО патча + вызов по адресу через
// интерфейс (исходник развёрнутого контракта не нужен, восстанавливаем из сигнатур)
struct ForkConfig {
    bool        enabled = false;
    std::string victim;              // адрес развёрнутой жертвы (0x...)
    std::string forkBlock;           // блок форка (до патча уязвимости)
    std::string rpcEnv = "RPC_URL";  // env-переменная с RPC-URL (ключ НЕ хардкодим)
};

// генератор PoC-эксплойтов (ров продукта, AD-7): по найденной reentrancy строит
// Foundry-тест, доказывающий кражу средств. без KLEE — привязанный к находке скаффолд
// (реальные конструктор/ABI/форк помечены TODO(hunter)). С KLEE-witness — путь кражи
// СИМВОЛЬНО ПОДТВЕРЖДЁН (не ложное срабатывание), спекулятивные TODO снимаются.
class PocGenerator {
public:
    // Foundry-PoC для reentrancy: атакующий контракт с реентрантным callback +
    // тест, проверяющий, что баланс жертвы упал, а атакующего — вырос.
    // contract — имя уязвимого контракта, f — находка (берём функцию),
    // w — witness-модель KLEE (по умолчанию не подтверждён).
    std::string generateReentrancy(const std::string& contract, const Finding& f,
                                   const KleeWitness& w = {}, const ForkConfig& fork = {});

    // Foundry-PoC для access-control: атакующий (не владелец) вызывает привилегированную
    // функцию; отсутствие revert доказывает отсутствие контроля доступа. С KLEE-witness —
    // символьно подтверждено (привилегированная запись достижима при msg.sender != owner).
    std::string generateAccessControl(const std::string& contract, const Finding& f,
                                      const KleeWitness& w = {}, const ForkConfig& fork = {});
};

}  // namespace solc::report
