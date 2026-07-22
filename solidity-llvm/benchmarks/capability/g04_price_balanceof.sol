// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: miss
// price-manipulation через balanceOf-соотношение (манипулируемо донатом/flash-loan). мы намеренно
// исключили balanceOf из oracle_read (FP-склонно) -> класс не детектится
interface IERC20 { function balanceOf(address) external view returns(uint256); }
contract C { IERC20 tokenA; IERC20 tokenB;
  function priceBuy() external payable {
    uint256 price = tokenA.balanceOf(address(this)) / tokenB.balanceOf(address(this));
    (bool ok,)=msg.sender.call{value: price}(""); require(ok); } }
