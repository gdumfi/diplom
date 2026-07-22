// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: miss
// unchecked ERC20 transfer (не-реверт токен возвращает false): перевод «прошёл», но не случился.
// у нас это только LOW unchecked-call, не HIGH/MEDIUM
interface IERC20 { function transfer(address,uint256) external returns(bool); }
contract C { IERC20 token; function pay(address to, uint256 a) external { token.transfer(to, a); } }
