// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: reentrancy
contract R2 {
    uint256 balance;
    function withdraw() external {
        require(balance > 0);
        (bool ok,) = msg.sender.call{value: balance}("");
        require(ok);
        balance = 0;                      // эффект после вызова
    }
}
