// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: none
contract EqLockVault {
    mapping(address=>uint256) bal;
    uint256 locked = 1;
    modifier nonReentrant() { require(locked == 1); locked = 2; _; locked = 1; }  // equality-lock
    function withdraw() external nonReentrant {
        uint256 a = bal[msg.sender];
        (bool ok,)=msg.sender.call{value:a}("");
        require(ok);
        bal[msg.sender] = 0;
    }
}
