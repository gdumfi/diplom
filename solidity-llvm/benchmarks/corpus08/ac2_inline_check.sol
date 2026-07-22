// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
contract Ownable {
    address public owner;
    constructor() { owner = msg.sender; }
    modifier onlyOwner() { require(msg.sender == owner, "not owner"); _; }
}
// expect: none
contract AC2 is Ownable {
    function setOwner(address n) external {
        require(msg.sender == owner, "denied");   // проверка в теле функции
        owner = n;
    }
}
