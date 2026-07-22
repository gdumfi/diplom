// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: none
contract KillSafe {
    address owner;
    function boom() external { require(msg.sender == owner); selfdestruct(payable(msg.sender)); }
}
