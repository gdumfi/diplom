// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// демо supply: УЯЗВИМАЯ эмиссия — mint публичный, без контроля прав (anyone can mint).
// supply-инвариант balanceOf(attacker)==0 должен СЛОМАТЬСЯ: атакующий чеканит себе.
contract Token {
    mapping(address => uint256) public balanceOf;
    uint256 public totalSupply;
    address public owner;

    constructor() { owner = msg.sender; }

    function mint(address to, uint256 a) external {   // нет onlyOwner — БАГ
        balanceOf[to] += a;
        totalSupply += a;
    }
}
