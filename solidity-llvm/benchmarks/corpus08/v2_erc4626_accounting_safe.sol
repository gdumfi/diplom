// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: none
contract Vault4626 {
    mapping(address=>uint256) public balanceOf;
    uint256 public totalSupply;
    address owner;
    function adminMint(uint256 amt) external { require(msg.sender == owner); totalSupply += amt; }
    function deposit(uint256 amt) external { balanceOf[msg.sender] += amt; totalSupply += amt; }  // permissionless учёт
    function redeem(uint256 amt) external { balanceOf[msg.sender] -= amt; totalSupply -= amt; }
}
