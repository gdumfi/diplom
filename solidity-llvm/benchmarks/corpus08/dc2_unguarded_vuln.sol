// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: delegatecall
// Проход 4a-контроль: delegatecall в attacker-контролируемый target БЕЗ проверки прав — HIGH остаётся.
// admin-гейт не должен подавлять реальный неограниченный delegatecall (исполнение чужого кода над storage)
contract Danger {
    function exec(address target, bytes calldata data) external {
        (bool ok, ) = target.delegatecall(data);
        require(ok);
    }
}
