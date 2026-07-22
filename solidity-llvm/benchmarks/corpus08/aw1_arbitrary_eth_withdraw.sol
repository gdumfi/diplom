// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
contract Ownable {
    address public owner;
    constructor() { owner = msg.sender; }
    modifier onlyOwner() { require(msg.sender == owner, "not owner"); _; }
}
// expect: access-control
// Fix4-контроль: неограниченный вывод ETH (value≠0) на ПРОИЗВОЛЬНЫЙ адрес без проверки — HIGH
// остаётся. фильтр value≠0 убирает FP на value=0 ERC20/flashLoan, но реальный ETH-вывод ловится
contract AW1 is Ownable {
    uint256 fee;
    function setFee(uint256 f) external onlyOwner { fee = f; }   // owner-проверка → эталон привилегии
    function withdrawTo(address to, uint256 amt) external {
        (bool ok, ) = to.call{value: amt}("");                   // произвольный адрес, value≠0
        require(ok);
    }
}
