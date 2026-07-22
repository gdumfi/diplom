// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
contract Vault {
    mapping(address => uint256) public bal;
    function deposit() external payable { bal[msg.sender] += msg.value; }
    function withdraw() external {
        uint256 a = bal[msg.sender];
        (bool ok,) = msg.sender.call{value: a}("");   // внешний вызов ДО обнуления
        require(ok);
        bal[msg.sender] = 0;
    }
}
