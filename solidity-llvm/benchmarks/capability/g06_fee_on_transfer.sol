// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: miss
// fee-on-transfer/accounting-delta: кредитуем amt, а не измеренную дельту balanceOf -> deflationary
// токен ломает учёт (кредит > реально полученного)
interface IERC20 { function transferFrom(address,address,uint256) external returns(bool); }
contract C { mapping(address=>uint256) bal; IERC20 token;
  function deposit(uint256 amt) external { token.transferFrom(msg.sender,address(this),amt); bal[msg.sender]+=amt; } }
