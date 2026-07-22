// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: reentrancy
interface IX { function poke() external; }   // имплементер вне файла -> MEDIUM (не подтверждён)
contract V {
    IX x;
    function withdraw(uint256 a) external {
        (bool ok,)=msg.sender.call{value:a}("");
        require(ok);
        x.poke();
    }
}
