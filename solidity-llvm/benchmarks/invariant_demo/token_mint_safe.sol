// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// демо supply: БЕЗОПАСНАЯ эмиссия — mint под onlyOwner. атакующий (не владелец) не может
// начеканить себе, balanceOf(attacker) остаётся 0 → инвариант ДЕРЖИТСЯ. тот же интерфейс.
contract Token {
    mapping(address => uint256) public balanceOf;
    uint256 public totalSupply;
    address public owner;

    constructor() { owner = msg.sender; }

    modifier onlyOwner() { require(msg.sender == owner, "not owner"); _; }

    function mint(address to, uint256 a) external onlyOwner {
        balanceOf[to] += a;
        totalSupply += a;
    }
}
