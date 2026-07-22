// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;

contract Ownable {
    address public owner;
    constructor() { owner = msg.sender; }
    modifier onlyOwner() { require(msg.sender == owner, "not owner"); _; }
}

struct UserInfo {
    uint256 amount;
    uint256 rewardDebt;
}

contract Vault is Ownable {
    mapping(address => uint256) public balances;
    mapping(address => UserInfo) public users;
    uint256 public totalSupply;

    function deposit() external payable {
        balances[msg.sender] += msg.value;
        users[msg.sender].amount += msg.value;
        totalSupply += msg.value;
    }

    function withdraw(uint256 amount) external {
        require(balances[msg.sender] >= amount, "insufficient");
        (bool ok, ) = msg.sender.call{value: amount}("");
        require(ok, "transfer failed");
        balances[msg.sender] -= amount;       // effect ПОСЛЕ вызова — reentrancy
    }

    function setOwner(address newOwner) external {
        owner = newOwner;                     // нет onlyOwner — access-control
    }
}
