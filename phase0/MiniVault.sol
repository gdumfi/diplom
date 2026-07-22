// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// мини-vault в духе реального инцидента: депозит/вывод, обнуление ПОСЛЕ выплаты
contract MiniVault {
    mapping(address => uint256) public balances;
    function deposit() external payable { balances[msg.sender] += msg.value; }
    function withdraw() external {
        uint256 bal = balances[msg.sender];
        require(bal > 0, "no funds");
        (bool ok, ) = msg.sender.call{value: bal}("");
        require(ok, "send failed");
        balances[msg.sender] = 0;            // эффект ПОСЛЕ вызова -> reentrancy
    }
    receive() external payable {}
}
