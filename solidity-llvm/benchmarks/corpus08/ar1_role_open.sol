// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: access-control
contract RoleBank {
    mapping(address => bool) public admins; address public treasury;
    function setGuarded(address t) external { require(admins[msg.sender], "no"); treasury = t; }
    function setOpen(address t) external { treasury = t; }   // без проверки
}
