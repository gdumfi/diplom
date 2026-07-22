// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: price-manipulation
interface IPair { function getReserves() external view returns (uint112,uint112,uint32); }
contract Lending {
    IPair pair;
    function borrow() external {
        (uint112 r0, uint112 r1,) = pair.getReserves();
        uint256 amount = (uint256(r0) * 100) / uint256(r1);
        (bool ok,) = msg.sender.call{value: amount}("");   // цена → отправка средств
        require(ok);
    }
}
