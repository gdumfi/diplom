#include <gtest/gtest.h>

#include <sstream>

#include "solc/report/Finding.h"
#include "solc/report/JsonReporter.h"

using solc::report::Finding;
using solc::report::Findings;
using solc::report::JsonReporter;

// пустой список находок даёт "[]"
TEST(JsonReporter, EmptyGivesEmptyArray) {
    Findings findings;
    std::ostringstream os;
    JsonReporter().report(findings, os);
    EXPECT_EQ(os.str(), "[]\n");
}

// одна находка печатает все поля в валидном JSON
TEST(JsonReporter, SingleFindingHasAllFields) {
    Finding f;
    f.rule = "reentrancy";
    f.severity = "HIGH";
    f.function = "withdraw";
    f.message = "storage write after external call";
    f.line = 42;
    f.storageSlot = 3;

    std::ostringstream os;
    JsonReporter().report(Findings{f}, os);
    const std::string out = os.str();

    EXPECT_NE(out.find("\"rule\": \"reentrancy\""), std::string::npos);
    EXPECT_NE(out.find("\"severity\": \"HIGH\""), std::string::npos);
    EXPECT_NE(out.find("\"function\": \"withdraw\""), std::string::npos);
    EXPECT_NE(out.find("\"line\": 42"), std::string::npos);
    EXPECT_NE(out.find("\"storageSlot\": 3"), std::string::npos);
    EXPECT_NE(out.find("\"message\": \"storage write after external call\""),
              std::string::npos);
    // массив с одним элементом — открывающая и закрывающая скобки на месте
    EXPECT_EQ(out.front(), '[');
    EXPECT_NE(out.find(']'), std::string::npos);
}

// бейдж символьного подтверждения (ров): symConfirmed → [SYM-CONFIRMED]/поле в JSON
TEST(JsonReporter, SymConfirmedBadge) {
    Finding f;
    f.rule = "reentrancy"; f.severity = "HIGH"; f.function = "withdraw";
    f.message = "reentrancy"; f.symConfirmed = true;
    std::ostringstream os;
    JsonReporter().report(Findings{f}, os);
    EXPECT_NE(os.str().find("\"symConfirmed\": true"), std::string::npos);

    Finding g = f; g.symConfirmed = false;
    std::ostringstream os2;
    JsonReporter().report(Findings{g}, os2);
    EXPECT_NE(os2.str().find("\"symConfirmed\": false"), std::string::npos);
}

// приоритет триажа выводится в JSON (для сортировки/фильтрации в тулинге)
TEST(JsonReporter, PriorityScoreEmitted) {
    Finding f;
    f.rule = "access-control"; f.severity = "HIGH"; f.function = "setOwner";
    f.message = "m"; f.priorityScore = 350;
    std::ostringstream os;
    JsonReporter().report(Findings{f}, os);
    EXPECT_NE(os.str().find("\"priorityScore\": 350"), std::string::npos);
}

// кавычки и обратные слэши в message корректно экранируются
TEST(JsonReporter, EscapesQuotesAndBackslashes) {
    Finding f;
    f.rule = "reentrancy";
    f.severity = "LOW";
    f.function = "f";
    f.message = "call \"foo\" via path\\to\\x";

    std::ostringstream os;
    JsonReporter().report(Findings{f}, os);
    const std::string out = os.str();

    // ожидаем экранированные кавычки \" и слэши \\ в выводе
    EXPECT_NE(out.find("\\\"foo\\\""), std::string::npos);
    EXPECT_NE(out.find("path\\\\to\\\\x"), std::string::npos);
}

#include "solc/report/PocGenerator.h"

TEST(PocGenerator, ReentrancyPocHasAttackAndAssertions) {
    solc::report::Finding f;
    f.rule = "reentrancy"; f.severity = "HIGH"; f.function = "withdraw"; f.line = 12;
    solc::report::PocGenerator pg;
    std::string poc = pg.generateReentrancy("Vault", f);

    // имя контракта и функции проброшены
    EXPECT_NE(poc.find("Vault"), std::string::npos);
    EXPECT_NE(poc.find("target.withdraw()"), std::string::npos);
    // реентрантный callback + точка входа
    EXPECT_NE(poc.find("receive() external payable"), std::string::npos);
    EXPECT_NE(poc.find("function attack()"), std::string::npos);
    // доказательство кражи — обе ассерции
    EXPECT_NE(poc.find("assertLt(address(victim).balance"), std::string::npos);
    EXPECT_NE(poc.find("assertGt(address(attacker).balance"), std::string::npos);
    // это Foundry-тест
    EXPECT_NE(poc.find("forge-std/Test.sol"), std::string::npos);
    // без KLEE-witness — best-effort каркас (не помечен подтверждённым)
    EXPECT_EQ(poc.find("ПОДТВЕРЖДЁН KLEE"), std::string::npos);
}

// K2: с KLEE-witness PoC помечается символьно подтверждённым и несёт witness-модель
TEST(PocGenerator, ConfirmedPocCarriesKleeWitness) {
    solc::report::Finding f;
    f.rule = "reentrancy"; f.severity = "HIGH"; f.function = "withdraw"; f.line = 14;
    solc::report::KleeWitness w;
    w.confirmed = true; w.balance = "1"; w.msgValue = "0"; w.depth = 1;
    solc::report::PocGenerator pg;
    std::string poc = pg.generateReentrancy("Vault", f, w);

    EXPECT_NE(poc.find("СИМВОЛЬНО ПОДТВЕРЖДЁН KLEE"), std::string::npos);
    EXPECT_NE(poc.find("witness-модель KLEE"), std::string::npos);
    EXPECT_NE(poc.find("balance >= 1 wei"), std::string::npos);
    // структура эксплойта сохранена
    EXPECT_NE(poc.find("receive() external payable"), std::string::npos);
    EXPECT_NE(poc.find("assertLt(address(victim).balance"), std::string::npos);
}

// witness-аргументы KLEE идут в шапку-комментарий, но НЕ подставляются в сам вызов
// (satisfying-модель часто огромна; рабочему PoC нужна сумма = депозит атакующего STEP)
TEST(PocGenerator, WitnessArgsInCommentNotInCall) {
    solc::report::Finding f;
    f.rule = "reentrancy"; f.severity = "HIGH"; f.function = "withdrawFunds"; f.line = 28;
    f.vulnParamTypes = {"uint256"};
    solc::report::KleeWitness w;
    w.confirmed = true; w.balance = "1000"; w.argValues = {"500"};
    solc::report::PocGenerator pg;
    std::string poc = pg.generateReentrancy("EtherStore", f, w);

    EXPECT_NE(poc.find("arg0=500"), std::string::npos);          // в комментарии
    EXPECT_NE(poc.find("withdrawFunds(STEP)"), std::string::npos); // вызов с STEP, не 500
    EXPECT_EQ(poc.find("withdrawFunds(500)"), std::string::npos);
}

// форк-режим reentrancy PoC: интерфейс жертвы из сигнатур + vm.createSelectFork по адресу
TEST(PocGenerator, ForkReentrancyTargetsDeployedVictim) {
    solc::report::Finding f;
    f.rule = "reentrancy"; f.severity = "HIGH"; f.function = "withdraw"; f.line = 14;
    solc::report::ForkConfig fork;
    fork.enabled = true; fork.victim = "0xABCD"; fork.forkBlock = "18000000";
    solc::report::PocGenerator pg;
    std::string poc = pg.generateReentrancy("Vault", f, {}, fork);

    EXPECT_NE(poc.find("interface IVault"), std::string::npos);         // интерфейс из сигнатур
    EXPECT_NE(poc.find("vm.createSelectFork"), std::string::npos);      // форк сети
    EXPECT_NE(poc.find("IVault(VICTIM)"), std::string::npos);           // жертва по адресу
    EXPECT_NE(poc.find("0xABCD"), std::string::npos);
    EXPECT_EQ(poc.find("new Vault("), std::string::npos);              // НЕ деплоим локально
}

// форк-режим access-control PoC
TEST(PocGenerator, ForkAccessControlTargetsDeployedVictim) {
    solc::report::Finding f;
    f.rule = "access-control"; f.severity = "HIGH"; f.function = "setOwner";
    f.vulnParamTypes = {"address"};
    solc::report::ForkConfig fork;
    fork.enabled = true; fork.victim = "0xDEAD"; fork.forkBlock = "17000000";
    solc::report::PocGenerator pg;
    std::string poc = pg.generateAccessControl("Bank", f, {}, fork);

    EXPECT_NE(poc.find("interface IBank"), std::string::npos);
    EXPECT_NE(poc.find("vm.createSelectFork"), std::string::npos);
    EXPECT_NE(poc.find("vm.prank(attacker)"), std::string::npos);
    EXPECT_NE(poc.find("victim.setOwner(attacker)"), std::string::npos);
}

// access-control PoC: атакующий (не владелец) вызывает привилегированную функцию;
// address-аргумент = attacker (забирает владение); с KLEE-witness помечен подтверждённым
TEST(PocGenerator, AccessControlPocAttackerCall) {
    solc::report::Finding f;
    f.rule = "access-control"; f.severity = "HIGH"; f.function = "setOwner";
    f.storageSlot = 0; f.vulnParamTypes = {"address"};
    solc::report::KleeWitness w; w.confirmed = true;
    solc::report::PocGenerator pg;
    std::string poc = pg.generateAccessControl("Bank", f, w);

    EXPECT_NE(poc.find("СИМВОЛЬНО ПОДТВЕРЖДЁН KLEE"), std::string::npos);
    EXPECT_NE(poc.find("vm.prank(attacker)"), std::string::npos);
    EXPECT_NE(poc.find("target.setOwner(attacker)"), std::string::npos);  // атакующий как новый owner
    EXPECT_NE(poc.find("forge-std/Test.sol"), std::string::npos);
}

// --- InvariantGen: стандартный ERC4626-харнесс имеет верные сигнатуры (регресс) ---
#include "solc/report/InvariantGen.h"

TEST(InvariantGen, Erc4626HarnessStandardSignatures) {
    solc::report::InvariantGen ig;
    std::string h = ig.generateInflationErc4626("MyVault");
    // стандартный интерфейс: deposit(assets,receiver) + redeem(shares,receiver,owner)
    EXPECT_NE(h.find("target.deposit(amt, address(this))"), std::string::npos);
    EXPECT_NE(h.find("target.redeem(myShares, address(this), address(this))"), std::string::npos);
    EXPECT_NE(h.find("target.deposit(amt, VICTIM)"), std::string::npos);
    // инвариант и имена контрактов на месте
    EXPECT_NE(h.find("invariant_noInflationProfit"), std::string::npos);
    EXPECT_NE(h.find("MyVaultErc4626InflationInvariant"), std::string::npos);
    EXPECT_NE(h.find("new MyVault(address(asset))"), std::string::npos);
}
