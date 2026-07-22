// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: reentrancy
contract R3 {
    mapping(address => uint256) public bal;
    function withdraw(uint256 amt) external {
        require(bal[msg.sender] >= amt);
        (bool ok,) = msg.sender.call{value: amt}("");
        require(ok);
        bal[msg.sender] -= amt;           // эффект после вызова
    }
}
