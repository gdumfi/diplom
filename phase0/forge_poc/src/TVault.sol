// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
contract TVault {
    mapping(address => uint128) public bal;
    function deposit(bytes32 note) external payable { bal[msg.sender] += uint128(msg.value); note; }
    function withdraw(uint128 amount) external {
        require(bal[msg.sender] >= amount);
        (bool ok,) = msg.sender.call{value: amount}("");
        require(ok);
        bal[msg.sender] -= amount;
    }
    receive() external payable {}
}
