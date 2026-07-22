// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: none
contract RoleBankSafe {
    mapping(address => bool) public admins; address public treasury;
    function setT(address t) external { require(admins[msg.sender]); treasury = t; }
}
