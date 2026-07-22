// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// read-only reentrancy, ETH-value триггер (класс Curve). getter price() отдаёт устаревший
// totalShares во время callback removeLiquidity. Должно ловиться (контроль).
contract Pool {
    uint256 public totalShares;
    function removeLiquidity(uint256 amt) external {
        (bool ok, ) = msg.sender.call{value: amt}("");
        require(ok);
        totalShares -= amt;                 // мутация ПОСЛЕ вызова
    }
    function price() external view returns (uint256) { return totalShares; }
}
