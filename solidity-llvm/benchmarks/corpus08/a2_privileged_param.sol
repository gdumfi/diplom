// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
contract Ownable {
    address public owner;
    constructor() { owner = msg.sender; }
    modifier onlyOwner() { require(msg.sender == owner, "not owner"); _; }
}
// expect: access-control
contract A2 is Ownable {
    uint256 public fee;
    function setFeeGuarded(uint256 f) external onlyOwner { fee = f; owner = owner; }
    function rugOwner(address n) external { owner = n; }          // смена owner без проверки
}
