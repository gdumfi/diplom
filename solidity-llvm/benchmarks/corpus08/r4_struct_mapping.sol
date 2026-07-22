// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: reentrancy
contract R4 {
    struct UserInfo { uint256 amount; uint256 rewardDebt; }
    mapping(address => UserInfo) public users;
    function deposit() external payable { users[msg.sender].amount += msg.value; }
    function withdraw() external {
        uint256 amt = users[msg.sender].amount;
        require(amt > 0);
        (bool ok,) = msg.sender.call{value: amt}("");
        require(ok);
        users[msg.sender].amount = 0;   // struct-поле после вызова -> reentrancy
    }
}
