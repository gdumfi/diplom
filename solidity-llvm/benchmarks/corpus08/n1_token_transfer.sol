// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: none
contract N1 {
    mapping(address => uint256) public balanceOf;
    function transfer(address to, uint256 a) external returns (bool) {
        require(balanceOf[msg.sender] >= a);
        balanceOf[msg.sender] -= a;
        balanceOf[to] += a;
        return true;
    }
}
