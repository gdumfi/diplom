// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: selfdestruct
contract Kill {
    uint256 public x;
    function setX(uint256 v) external { require(msg.sender != address(0)); x = v; }
    function boom() external { selfdestruct(payable(msg.sender)); }
}
