// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: none
contract N2 {
    uint256 public total;
    mapping(address => uint256) public bal;
    function balanceOf(address a) external view returns (uint256) { return bal[a]; }
    function totalSupply() external view returns (uint256) { return total; }
}
