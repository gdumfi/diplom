// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: none
contract S4 {
    struct UserInfo { uint256 amount; uint256 rewardDebt; }
    mapping(address => UserInfo) public users;
    function withdraw() external {
        uint256 amt = users[msg.sender].amount;
        users[msg.sender].amount = 0;   // эффект ДО вызова (CEI)
        (bool ok,) = msg.sender.call{value: amt}("");
        require(ok);
    }
}
