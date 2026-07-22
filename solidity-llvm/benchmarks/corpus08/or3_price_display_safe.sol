// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: none
interface IPair { function getReserves() external view returns (uint112,uint112,uint32); }
contract Safe2 {
    IPair pair;
    function shownReserve() external view returns (uint256) {
        (uint112 r0,,) = pair.getReserves();               // читаем, но НЕ шлём средства
        return uint256(r0);
    }
}
