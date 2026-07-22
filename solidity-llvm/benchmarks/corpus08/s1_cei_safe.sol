// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: none
contract S1 {
    mapping(address => uint256) public bal;
    function withdraw() external {
        uint256 a = bal[msg.sender];
        bal[msg.sender] = 0;              // эффект ДО вызова (CEI)
        (bool ok,) = msg.sender.call{value: a}("");
        require(ok);
    }
}
