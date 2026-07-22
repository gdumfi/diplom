// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: none
contract S2 {
    mapping(address => uint256) public bal;
    function move(address to, uint256 a) external {
        require(bal[msg.sender] >= a);
        bal[msg.sender] -= a;
        bal[to] += a;                      // нет внешнего вызова
    }
}
