// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
contract Ownable {
    address public owner;
    constructor() { owner = msg.sender; }
    modifier onlyOwner() { require(msg.sender == owner, "not owner"); _; }
}
contract Bank is Ownable {
    mapping(address => uint256) public balances;
    // защищена модификатором — access-control детектор НЕ должен флагать (guard инлайнится)
    function setOwnerGuarded(address n) external onlyOwner { owner = n; }
    // НЕ защищена — детектор ДОЛЖЕН флагать
    function setOwnerOpen(address n) external { owner = n; }
    function withdraw(uint256 a) external {
        require(balances[msg.sender] >= a);
        (bool ok,) = msg.sender.call{value: a}("");
        require(ok);
        balances[msg.sender] -= a;   // reentrancy
    }
}
