#include "solc/report/InvariantGen.h"

#include <sstream>
#include <vector>

namespace solc::report {

namespace {

// типизированная заглушка аргумента (совпадает по духу с PocGenerator): чтобы харнесс
// компилировался. uint → STEP; узкие целые кастуем; address/bool/bytes → нулевые
std::string defaultArg(const std::string& ty) {
    if (ty == "address")                    return "address(this)";
    if (ty == "bool")                       return "true";
    if (ty == "string" || ty == "bytes")    return "\"\"";
    if (ty.rfind("bytes", 0) == 0)          return ty + "(0)";
    if (ty.find("[]") != std::string::npos) return "new " + ty + "(0)";
    if (ty == "uint256" || ty == "uint")    return "STEP";
    if (ty.rfind("uint", 0) == 0 || ty.rfind("int", 0) == 0)
        return ty + "(STEP)";
    return "STEP";
}

std::string argList(const std::vector<std::string>& types) {
    std::string s;
    for (size_t i = 0; i < types.size(); ++i) { if (i) s += ", "; s += defaultArg(types[i]); }
    return s;
}

}  // namespace

std::string InvariantGen::generateSolvency(const std::string& contract, const Finding& f) {
    const std::string& vuln = f.function;              // функция вывода (напр. withdraw)
    const std::string& fund = f.fundingFunction;       // функция депозита (payable), может быть пустой
    const std::string vulnArgs = argList(f.vulnParamTypes);
    const std::string fundArgs = argList(f.fundingParamTypes);
    std::ostringstream os;

    os << "// SPDX-License-Identifier: UNLICENSED\n";
    os << "// Инвариант-харнесс SOLVENCY (no-overdraw), сгенерирован SCSA (Фаза B).\n";
    os << "// цель: " << contract << " — depozit `" << (fund.empty() ? "?" : fund)
       << "`, вывод `" << vuln << "`";
    if (f.line > 0) os << " (строка " << f.line << ")";
    os << "\n";
    os << "// свойство: атакующий не может вывести больше, чем внёс (иначе крадёт засеянный\n";
    os << "//           чужой TVL). ломается на reentrancy И на битом учёте/округлении.\n";
    os << "// запуск: forge test --match-contract " << contract << "SolvencyInvariant -vvv\n";
    if (fund.empty())
        os << "// TODO(hunter): depozit-функция не выведена статически — впиши её в h_deposit.\n";
    os << "pragma solidity ^0.8.13;\n\n";
    os << "import \"forge-std/Test.sol\";\n";
    os << "import {" << contract << "} from \"../src/" << contract << ".sol\";\n\n";

    // --- Handler: атакующий-актор, которого фаззит forge ---
    os << "// Handler — атакующий-актор. forge дёргает его h_* в случайных\n";
    os << "// последовательностях; реентрантный receive повторяет вывод (глубина <= MAX_DEPTH).\n";
    os << "contract " << contract << "Handler is Test {\n";
    os << "    " << contract << " public target;\n";
    os << "    uint256 public constant STEP = 1 ether;\n";
    os << "    uint256 public constant MAX_DEPTH = 3;\n";
    os << "    uint256 public deposited;   // Σ внесённого атакующим\n";
    os << "    uint256 public withdrawn;   // Σ выведенного атакующим (в т.ч. реентрантно)\n";
    os << "    uint256 private _depth;\n\n";
    os << "    constructor(" << contract << " _t) { target = _t; }\n\n";
    os << "    function h_deposit(uint256 amt) public {\n";
    os << "        amt = bound(amt, 0.01 ether, 10 ether);\n";
    os << "        vm.deal(address(this), address(this).balance + amt);\n";
    if (!fund.empty()) {
        os << "        try target." << fund << "{value: amt}(" << fundArgs << ") {\n";
        os << "            deposited += amt;\n";
        os << "        } catch {}\n";
    } else {
        os << "        // TODO(hunter): вызов depozit-функции с {value: amt}\n";
    }
    os << "    }\n\n";
    os << "    function h_withdraw() public {\n";
    os << "        uint256 pre = address(this).balance;\n";
    os << "        try target." << vuln << "(" << vulnArgs << ") {} catch {}\n";
    os << "        uint256 post = address(this).balance;\n";
    os << "        if (post > pre) withdrawn += post - pre;   // прирост = выведенное\n";
    os << "    }\n\n";
    os << "    // реентрантный callback: пока есть что тянуть и глубина не исчерпана — заходим снова\n";
    os << "    receive() external payable {\n";
    os << "        if (_depth < MAX_DEPTH && address(target).balance >= STEP) {\n";
    os << "            _depth++;\n";
    os << "            try target." << vuln << "(" << vulnArgs << ") {} catch {}\n";
    os << "            _depth--;\n";
    os << "        }\n";
    os << "    }\n";
    os << "}\n\n";

    // --- инвариант-тест ---
    os << "contract " << contract << "SolvencyInvariant is Test {\n";
    os << "    " << contract << " target;\n";
    os << "    " << contract << "Handler handler;\n";
    os << "    uint256 constant SEED = 100 ether;   // чужой TVL в пуле (средства других юзеров)\n\n";
    os << "    function setUp() public {\n";
    os << "        // TODO(hunter): для боевого протокола — vm.createSelectFork(RPC, block) + адрес жертвы\n";
    os << "        target = new " << contract << "();\n";
    os << "        vm.deal(address(target), SEED);   // засеваем «чужие» средства — их и попытается украсть handler\n";
    os << "        handler = new " << contract << "Handler(target);\n";
    os << "        targetContract(address(handler));\n";
    os << "    }\n\n";
    os << "    // атакующий НЕ должен вывести больше, чем внёс. если смог (reentrancy / битый\n";
    os << "    // учёт / округление) — withdrawn > deposited, инвариант падает, forge даёт\n";
    os << "    // последовательность-контрпример = подтверждённый эксплойт.\n";
    os << "    function invariant_noOverdraw() public view {\n";
    os << "        assertLe(handler.withdrawn(), handler.deposited(), \"attacker overdrew: TVL theft\");\n";
    os << "    }\n";
    os << "}\n";

    return os.str();
}

std::string InvariantGen::generateInflation(const std::string& contract) {
    const std::string H = contract + "InflationHandler";
    std::ostringstream os;

    os << "// SPDX-License-Identifier: UNLICENSED\n";
    os << "// Инвариант-харнесс FIRST-DEPOSITOR / SHARE-INFLATION, сгенерирован SCSA (Фаза B+).\n";
    os << "// свойство: атакующий не выводит больше, чем внёс+донатил — иначе он украл депозит\n";
    os << "//           жертвы через округление её долей в 0 (инфляция цены доли донатом).\n";
    os << "// допущение формы: deposit() payable returns(uint256), redeem(uint256) returns(uint256).\n";
    os << "//   для ERC20-ERC4626 (asset/approve/receiver) адаптируй интерфейс — честная граница.\n";
    os << "// запуск: forge test --match-contract " << contract << "InflationInvariant -vvv\n";
    os << "pragma solidity ^0.8.13;\n\n";
    os << "import \"forge-std/Test.sol\";\n";
    os << "import {" << contract << "} from \"../src/" << contract << ".sol\";\n\n";

    // --- Handler: атакующий-актор + действие «жертва вносит» ---
    os << "contract " << H << " is Test {\n";
    os << "    " << contract << " public target;\n";
    os << "    address constant VICTIM = address(0xCAFE);\n";
    os << "    uint256 public deposited;   // атакующий внёс через vault\n";
    os << "    uint256 public donated;     // атакующий донатил напрямую (force-send)\n";
    os << "    uint256 public withdrawn;   // атакующий вывел\n";
    os << "    uint256 public myShares;    // доли атакующего (из возврата deposit)\n\n";
    os << "    constructor(" << contract << " _t) { target = _t; }\n";
    os << "    receive() external payable {}\n\n";
    os << "    // атакующий вносит КРОШЕЧНО — чтобы стать первым вкладчиком с 1 долей\n";
    os << "    function h_deposit(uint256 amt) public {\n";
    os << "        amt = bound(amt, 1, 1000);\n";
    os << "        vm.deal(address(this), address(this).balance + amt);\n";
    os << "        try target.deposit{value: amt}() returns (uint256 s) {\n";
    os << "            deposited += amt; myShares += s;\n";
    os << "        } catch {}\n";
    os << "    }\n\n";
    os << "    // донат напрямую в пул — раздувает цену доли (force-send неизбежен)\n";
    os << "    function h_donate(uint256 amt) public {\n";
    os << "        amt = bound(amt, 0.1 ether, 200 ether);\n";
    os << "        vm.deal(address(target), address(target).balance + amt);\n";
    os << "        donated += amt;\n";
    os << "    }\n\n";
    os << "    // жертва вносит нормальную сумму (её доли могут округлиться в 0)\n";
    os << "    function h_victimDeposit(uint256 amt) public {\n";
    os << "        amt = bound(amt, 0.01 ether, 10 ether);\n";
    os << "        vm.deal(VICTIM, amt);\n";
    os << "        vm.prank(VICTIM);\n";
    os << "        try target.deposit{value: amt}() returns (uint256) {} catch {}\n";
    os << "    }\n\n";
    os << "    // атакующий выкупает свои доли — забирает всё, включая депозит жертвы\n";
    os << "    function h_redeem() public {\n";
    os << "        if (myShares == 0) return;\n";
    os << "        uint256 pre = address(this).balance;\n";
    os << "        try target.redeem(myShares) returns (uint256) { myShares = 0; } catch {}\n";
    os << "        uint256 post = address(this).balance;\n";
    os << "        if (post > pre) withdrawn += post - pre;\n";
    os << "    }\n";
    os << "}\n\n";

    // --- инвариант-тест ---
    os << "contract " << contract << "InflationInvariant is Test {\n";
    os << "    " << contract << " target;\n";
    os << "    " << H << " handler;\n\n";
    os << "    function setUp() public {\n";
    os << "        target = new " << contract << "();\n";
    os << "        handler = new " << H << "(target);\n";
    os << "        targetContract(address(handler));\n";
    os << "    }\n\n";
    os << "    // атакующий не должен получить больше, чем внёс+донатил. если получил —\n";
    os << "    // он украл депозит жертвы через share-inflation, forge даёт контрпример.\n";
    os << "    function invariant_noInflationProfit() public view {\n";
    os << "        assertLe(handler.withdrawn(), handler.deposited() + handler.donated(),\n";
    os << "                 \"share-inflation: attacker stole victim deposit\");\n";
    os << "    }\n";
    os << "}\n";

    return os.str();
}

std::string InvariantGen::generateInflationErc20(const std::string& contract) {
    const std::string H = contract + "Erc20InflationHandler";
    std::ostringstream os;

    os << "// SPDX-License-Identifier: UNLICENSED\n";
    os << "// Инвариант-харнесс ERC4626 FIRST-DEPOSITOR / SHARE-INFLATION (над ERC20), SCSA (Фаза B+).\n";
    os << "// реальная форма: asset/transferFrom/approve. донат = asset.transfer в пул раздувает\n";
    os << "// цену доли → депозит жертвы округляется в 0 долей → атакующий выкупает всё.\n";
    os << "// свойство: withdrawn(assets) <= deposited + donated (не украл депозит жертвы).\n";
    os << "// допущение формы: constructor(address asset), deposit(uint256) returns(uint256),\n";
    os << "//   redeem(uint256) returns(uint256). MockERC20 — inline (харнесс самодостаточен).\n";
    os << "// запуск: forge test --match-contract " << contract << "Erc20InflationInvariant -vvv\n";
    os << "pragma solidity ^0.8.13;\n\n";
    os << "import \"forge-std/Test.sol\";\n";
    os << "import {" << contract << "} from \"../src/" << contract << ".sol\";\n\n";

    // --- inline минимальный ERC20-актив ---
    os << "// минимальный mintable ERC20 — актив vault-а (только для харнесса)\n";
    os << "contract MockERC20 {\n";
    os << "    mapping(address => uint256) public balanceOf;\n";
    os << "    mapping(address => mapping(address => uint256)) public allowance;\n";
    os << "    function mint(address to, uint256 a) external { balanceOf[to] += a; }\n";
    os << "    function approve(address s, uint256 a) external returns (bool) { allowance[msg.sender][s] = a; return true; }\n";
    os << "    function transfer(address to, uint256 a) external returns (bool) {\n";
    os << "        balanceOf[msg.sender] -= a; balanceOf[to] += a; return true; }\n";
    os << "    function transferFrom(address f, address to, uint256 a) external returns (bool) {\n";
    os << "        if (allowance[f][msg.sender] != type(uint256).max) allowance[f][msg.sender] -= a;\n";
    os << "        balanceOf[f] -= a; balanceOf[to] += a; return true; }\n";
    os << "}\n\n";

    // --- Handler ---
    os << "contract " << H << " is Test {\n";
    os << "    " << contract << " public target;\n";
    os << "    MockERC20 public asset;\n";
    os << "    address constant VICTIM = address(0xCAFE);\n";
    os << "    uint256 public deposited;\n";
    os << "    uint256 public donated;\n";
    os << "    uint256 public withdrawn;\n";
    os << "    uint256 public myShares;\n\n";
    os << "    constructor(" << contract << " _t, MockERC20 _a) {\n";
    os << "        target = _t; asset = _a;\n";
    os << "        asset.approve(address(target), type(uint256).max);\n";
    os << "    }\n\n";
    os << "    function h_deposit(uint256 amt) public {\n";
    os << "        amt = bound(amt, 1, 1000);\n";
    os << "        asset.mint(address(this), amt);\n";
    os << "        try target.deposit(amt) returns (uint256 s) { deposited += amt; myShares += s; } catch {}\n";
    os << "    }\n\n";
    os << "    // донат актива напрямую в пул — раздувает цену доли\n";
    os << "    function h_donate(uint256 amt) public {\n";
    os << "        amt = bound(amt, 0.1 ether, 200 ether);\n";
    os << "        asset.mint(address(this), amt);\n";
    os << "        asset.transfer(address(target), amt);\n";
    os << "        donated += amt;\n";
    os << "    }\n\n";
    os << "    function h_victimDeposit(uint256 amt) public {\n";
    os << "        amt = bound(amt, 0.01 ether, 10 ether);\n";
    os << "        asset.mint(VICTIM, amt);\n";
    os << "        vm.startPrank(VICTIM);\n";
    os << "        asset.approve(address(target), type(uint256).max);\n";
    os << "        try target.deposit(amt) returns (uint256) {} catch {}\n";
    os << "        vm.stopPrank();\n";
    os << "    }\n\n";
    os << "    function h_redeem() public {\n";
    os << "        if (myShares == 0) return;\n";
    os << "        uint256 pre = asset.balanceOf(address(this));\n";
    os << "        try target.redeem(myShares) returns (uint256) { myShares = 0; } catch {}\n";
    os << "        uint256 post = asset.balanceOf(address(this));\n";
    os << "        if (post > pre) withdrawn += post - pre;\n";
    os << "    }\n";
    os << "}\n\n";

    // --- инвариант-тест ---
    os << "contract " << contract << "Erc20InflationInvariant is Test {\n";
    os << "    " << contract << " target;\n";
    os << "    MockERC20 asset;\n";
    os << "    " << H << " handler;\n\n";
    os << "    function setUp() public {\n";
    os << "        asset = new MockERC20();\n";
    os << "        target = new " << contract << "(address(asset));\n";
    os << "        handler = new " << H << "(target, asset);\n";
    os << "        targetContract(address(handler));\n";
    os << "    }\n\n";
    os << "    function invariant_noInflationProfit() public view {\n";
    os << "        assertLe(handler.withdrawn(), handler.deposited() + handler.donated(),\n";
    os << "                 \"ERC4626 share-inflation: attacker stole victim deposit\");\n";
    os << "    }\n";
    os << "}\n";

    return os.str();
}

std::string InvariantGen::generateInflationErc4626(const std::string& contract) {
    const std::string H = contract + "Erc4626InflationHandler";
    std::ostringstream os;

    os << "// SPDX-License-Identifier: UNLICENSED\n";
    os << "// Инвариант-харнесс СТАНДАРТНЫЙ ERC4626 first-depositor/share-inflation, SCSA (Фаза B+).\n";
    os << "// применим к боевым vault-ам: deposit(assets,receiver), redeem(shares,receiver,owner).\n";
    os << "// свойство: withdrawn(assets) <= deposited + donated (не украл депозит жертвы).\n";
    os << "// допущение формы: constructor(address asset). запуск:\n";
    os << "// forge test --match-contract " << contract << "Erc4626InflationInvariant -vvv\n";
    os << "pragma solidity ^0.8.13;\n\n";
    os << "import \"forge-std/Test.sol\";\n";
    os << "import {" << contract << "} from \"../src/" << contract << ".sol\";\n\n";

    os << "// минимальный mintable ERC20 — актив vault-а (только для харнесса)\n";
    os << "contract MockERC20 {\n";
    os << "    mapping(address => uint256) public balanceOf;\n";
    os << "    mapping(address => mapping(address => uint256)) public allowance;\n";
    os << "    function mint(address to, uint256 a) external { balanceOf[to] += a; }\n";
    os << "    function approve(address s, uint256 a) external returns (bool) { allowance[msg.sender][s] = a; return true; }\n";
    os << "    function transfer(address to, uint256 a) external returns (bool) {\n";
    os << "        balanceOf[msg.sender] -= a; balanceOf[to] += a; return true; }\n";
    os << "    function transferFrom(address f, address to, uint256 a) external returns (bool) {\n";
    os << "        if (allowance[f][msg.sender] != type(uint256).max) allowance[f][msg.sender] -= a;\n";
    os << "        balanceOf[f] -= a; balanceOf[to] += a; return true; }\n";
    os << "}\n\n";

    os << "contract " << H << " is Test {\n";
    os << "    " << contract << " public target;\n";
    os << "    MockERC20 public asset;\n";
    os << "    address constant VICTIM = address(0xCAFE);\n";
    os << "    uint256 public deposited;\n";
    os << "    uint256 public donated;\n";
    os << "    uint256 public withdrawn;\n";
    os << "    uint256 public myShares;\n\n";
    os << "    constructor(" << contract << " _t, MockERC20 _a) {\n";
    os << "        target = _t; asset = _a;\n";
    os << "        asset.approve(address(target), type(uint256).max);\n";
    os << "    }\n\n";
    os << "    function h_deposit(uint256 amt) public {\n";
    os << "        amt = bound(amt, 1, 1000);\n";
    os << "        asset.mint(address(this), amt);\n";
    os << "        try target.deposit(amt, address(this)) returns (uint256 s) { deposited += amt; myShares += s; } catch {}\n";
    os << "    }\n\n";
    os << "    // донат актива напрямую в пул — раздувает цену доли\n";
    os << "    function h_donate(uint256 amt) public {\n";
    os << "        amt = bound(amt, 0.1 ether, 200 ether);\n";
    os << "        asset.mint(address(this), amt);\n";
    os << "        asset.transfer(address(target), amt);\n";
    os << "        donated += amt;\n";
    os << "    }\n\n";
    os << "    function h_victimDeposit(uint256 amt) public {\n";
    os << "        amt = bound(amt, 0.01 ether, 10 ether);\n";
    os << "        asset.mint(VICTIM, amt);\n";
    os << "        vm.startPrank(VICTIM);\n";
    os << "        asset.approve(address(target), type(uint256).max);\n";
    os << "        try target.deposit(amt, VICTIM) returns (uint256) {} catch {}\n";
    os << "        vm.stopPrank();\n";
    os << "    }\n\n";
    os << "    function h_redeem() public {\n";
    os << "        if (myShares == 0) return;\n";
    os << "        uint256 pre = asset.balanceOf(address(this));\n";
    os << "        try target.redeem(myShares, address(this), address(this)) returns (uint256) { myShares = 0; } catch {}\n";
    os << "        uint256 post = asset.balanceOf(address(this));\n";
    os << "        if (post > pre) withdrawn += post - pre;\n";
    os << "    }\n";
    os << "}\n\n";

    os << "contract " << contract << "Erc4626InflationInvariant is Test {\n";
    os << "    " << contract << " target;\n";
    os << "    MockERC20 asset;\n";
    os << "    " << H << " handler;\n\n";
    os << "    function setUp() public {\n";
    os << "        asset = new MockERC20();\n";
    os << "        target = new " << contract << "(address(asset));\n";
    os << "        handler = new " << H << "(target, asset);\n";
    os << "        targetContract(address(handler));\n";
    os << "    }\n\n";
    os << "    function invariant_noInflationProfit() public view {\n";
    os << "        assertLe(handler.withdrawn(), handler.deposited() + handler.donated(),\n";
    os << "                 \"ERC4626 share-inflation: attacker stole victim deposit\");\n";
    os << "    }\n";
    os << "}\n";

    return os.str();
}

std::string InvariantGen::generateRoundtrip(const std::string& contract, const ForkConfig& fork) {
    const std::string H = contract + "RoundtripHandler";
    std::ostringstream os;

    // ---- ФОРК-режим: бьёмся о РЕАЛЬНЫЙ ERC4626 по адресу (охота по scope баунти-программы) ----
    // view-only round-trip округления работает при ЛЮБОМ состоянии живого пула (не требует
    // пустого vault, как inflation). не деплоим — только интерфейс + createSelectFork.
    if (fork.enabled) {
        const std::string FH = contract + "RoundtripForkHandler";
        os << "// SPDX-License-Identifier: UNLICENSED\n";
        os << "// ROUND-TRIP ОКРУГЛЕНИЯ — ФОРК-режим (реальная жертва по адресу), SCSA (Фаза B+).\n";
        os << "// свойство: convertToAssets(convertToShares(x)) <= x на ЖИВОМ состоянии пула.\n";
        os << "// цель: " << fork.victim << " @ блок " << fork.forkBlock << " · форма: ERC4626 view.\n";
        os << "// запуск: " << fork.rpcEnv << "=<rpc-url> forge test --match-contract "
           << contract << "RoundtripForkTest -vvv\n";
        os << "pragma solidity ^0.8.13;\n\n";
        os << "import \"forge-std/Test.sol\";\n\n";
        os << "interface IVault {\n";
        os << "    function convertToShares(uint256) external view returns (uint256);\n";
        os << "    function convertToAssets(uint256) external view returns (uint256);\n";
        os << "}\n\n";
        os << "contract " << FH << " is Test {\n";
        os << "    IVault public target;\n";
        os << "    bool public violated;\n";
        os << "    uint256 public badX;\n";
        os << "    uint256 public badBack;\n\n";
        os << "    constructor(IVault _t) { target = _t; }\n\n";
        os << "    function h_probe(uint256 x) public {\n";
        os << "        x = bound(x, 1, 1e18);\n";
        os << "        try target.convertToAssets(target.convertToShares(x)) returns (uint256 back) {\n";
        os << "            if (back > x) { violated = true; badX = x; badBack = back; }\n";
        os << "        } catch {}\n";
        os << "    }\n";
        os << "}\n\n";
        os << "contract " << contract << "RoundtripForkTest is Test {\n";
        os << "    IVault target;\n";
        os << "    " << FH << " handler;\n";
        os << "    address constant VICTIM = " << fork.victim << ";\n";
        // block 0/пусто → форк на latest (текущее состояние; не нужен архивный RPC).
        // конкретный блок → архивный запрос (для «до патча»); нужен archive-провайдер
        const bool latest = fork.forkBlock.empty() || fork.forkBlock == "0";
        if (!latest)
            os << "    uint256 constant FORK_BLOCK = " << fork.forkBlock << ";\n";
        os << "\n    function setUp() public {\n";
        if (latest)
            os << "        vm.createSelectFork(vm.envString(\"" << fork.rpcEnv << "\"));  // latest\n";
        else
            os << "        vm.createSelectFork(vm.envString(\"" << fork.rpcEnv << "\"), FORK_BLOCK);\n";
        os << "        target = IVault(VICTIM);\n";
        os << "        handler = new " << FH << "(target);\n";
        os << "        targetContract(address(handler));\n";
        os << "    }\n\n";
        os << "    function invariant_roundtripNoValueCreation() public view {\n";
        os << "        assertFalse(handler.violated(),\n";
        os << "                    \"rounding creates value on live vault: convertToAssets(convertToShares(x)) > x\");\n";
        os << "    }\n";
        os << "}\n";
        return os.str();
    }

    os << "// SPDX-License-Identifier: UNLICENSED\n";
    os << "// Инвариант-харнесс ROUND-TRIP ОКРУГЛЕНИЯ, сгенерирован SCSA (Фаза B+).\n";
    os << "// свойство: convertToAssets(convertToShares(x)) <= x ВСЕГДА — иначе округление\n";
    os << "//           создаёт стоимость (класс Balancer-2025, $100M на направлении округления).\n";
    os << "// setUp сеет раздутый курс (1 доля + донат актива), Handler фаззит x.\n";
    os << "// допущение формы: constructor(address), deposit(uint256), convertToShares(uint256) view,\n";
    os << "//   convertToAssets(uint256) view. MockERC20 — inline.\n";
    os << "// запуск: forge test --match-contract " << contract << "RoundtripInvariant -vvv\n";
    os << "pragma solidity ^0.8.13;\n\n";
    os << "import \"forge-std/Test.sol\";\n";
    os << "import {" << contract << "} from \"../src/" << contract << ".sol\";\n\n";

    os << "contract MockERC20 {\n";
    os << "    mapping(address => uint256) public balanceOf;\n";
    os << "    mapping(address => mapping(address => uint256)) public allowance;\n";
    os << "    function mint(address to, uint256 a) external { balanceOf[to] += a; }\n";
    os << "    function approve(address s, uint256 a) external returns (bool) { allowance[msg.sender][s] = a; return true; }\n";
    os << "    function transferFrom(address f, address to, uint256 a) external returns (bool) {\n";
    os << "        if (allowance[f][msg.sender] != type(uint256).max) allowance[f][msg.sender] -= a;\n";
    os << "        balanceOf[f] -= a; balanceOf[to] += a; return true; }\n";
    os << "}\n\n";

    // Handler — фаззит x, флагует нарушение round-trip
    os << "contract " << H << " is Test {\n";
    os << "    " << contract << " public target;\n";
    os << "    bool public violated;\n";
    os << "    uint256 public badX;\n";
    os << "    uint256 public badBack;\n\n";
    os << "    constructor(" << contract << " _t) { target = _t; }\n\n";
    os << "    function h_probe(uint256 x) public {\n";
    os << "        x = bound(x, 1, 1e15);\n";
    os << "        uint256 back = target.convertToAssets(target.convertToShares(x));\n";
    os << "        if (back > x) { violated = true; badX = x; badBack = back; }\n";
    os << "    }\n";
    os << "}\n\n";

    os << "contract " << contract << "RoundtripInvariant is Test {\n";
    os << "    " << contract << " target;\n";
    os << "    MockERC20 asset;\n";
    os << "    " << H << " handler;\n\n";
    os << "    function setUp() public {\n";
    os << "        asset = new MockERC20();\n";
    os << "        target = new " << contract << "(address(asset));\n";
    os << "        // сеем раздутый курс: 1 доля, затем крупный донат актива напрямую\n";
    os << "        asset.mint(address(this), 1);\n";
    os << "        asset.approve(address(target), type(uint256).max);\n";
    os << "        target.deposit(1);\n";
    os << "        asset.mint(address(target), 100 ether);\n";
    os << "        handler = new " << H << "(target);\n";
    os << "        targetContract(address(handler));\n";
    os << "    }\n\n";
    os << "    // округление не должно создавать стоимость ни при каком x\n";
    os << "    function invariant_roundtripNoValueCreation() public view {\n";
    os << "        assertFalse(handler.violated(),\n";
    os << "                    \"rounding creates value: convertToAssets(convertToShares(x)) > x\");\n";
    os << "    }\n";
    os << "}\n";

    return os.str();
}

std::string InvariantGen::generateSupply(const std::string& contract) {
    const std::string H = contract + "SupplyHandler";
    std::ostringstream os;

    os << "// SPDX-License-Identifier: UNLICENSED\n";
    os << "// Инвариант-харнесс SUPPLY-INTEGRITY (unauthorized-mint), сгенерирован SCSA (Фаза B+).\n";
    os << "// свойство: balanceOf(attacker) == 0 — атакующий (не владелец), который ничего не вносил,\n";
    os << "// не должен получить баланс. ломается на «anyone can mint» / битом контроле эмиссии.\n";
    os << "// допущение формы: mint(address,uint256), balanceOf(address) view. handler = attacker\n";
    os << "// (msg.sender != owner-деплойера теста).\n";
    os << "// запуск: forge test --match-contract " << contract << "SupplyInvariant -vvv\n";
    os << "pragma solidity ^0.8.13;\n\n";
    os << "import \"forge-std/Test.sol\";\n";
    os << "import {" << contract << "} from \"../src/" << contract << ".sol\";\n\n";

    os << "contract " << H << " is Test {\n";
    os << "    " << contract << " public target;\n";
    os << "    constructor(" << contract << " _t) { target = _t; }\n\n";
    os << "    // атакующий фаззит точку эмиссии на себя\n";
    os << "    function h_mint(uint256 amt) public {\n";
    os << "        amt = bound(amt, 1, 1e24);\n";
    os << "        try target.mint(address(this), amt) {} catch {}\n";
    os << "    }\n";
    os << "}\n\n";

    os << "contract " << contract << "SupplyInvariant is Test {\n";
    os << "    " << contract << " target;\n";
    os << "    " << H << " handler;\n\n";
    os << "    function setUp() public {\n";
    os << "        target = new " << contract << "();     // тест — владелец/деплойер\n";
    os << "        handler = new " << H << "(target);       // handler — НЕ владелец\n";
    os << "        targetContract(address(handler));\n";
    os << "    }\n\n";
    os << "    // атакующий не должен разжиться балансом из ничего\n";
    os << "    function invariant_noUnauthorizedMint() public view {\n";
    os << "        assertEq(target.balanceOf(address(handler)), 0,\n";
    os << "                 \"unauthorized mint: attacker credited without authority\");\n";
    os << "    }\n";
    os << "}\n";

    return os.str();
}

}  // namespace solc::report
