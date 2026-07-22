// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: reentrancy
// Fix6-контроль: если внутренний helper РЕАЛЬНО шлёт ETH-value, highCapable пропагируется вверх и
// классическая reentrancy остаётся HIGH (Fix6 не должен подавлять настоящий ETH-drain через вызов)
contract Vault {
    mapping(address => uint256) bal;
    function deposit() external payable { bal[msg.sender] += msg.value; }
    function sendIt(uint256 a) internal { (bool ok,) = msg.sender.call{value: a}(""); require(ok); }
    function withdraw() external {
        uint256 a = bal[msg.sender];
        sendIt(a);              // внутренний вызов, отправляющий ETH → highCapable
        bal[msg.sender] = 0;    // запись эффекта ПОСЛЕ — reentrancy
    }
}
