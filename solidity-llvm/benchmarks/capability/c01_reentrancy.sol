// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: reentrancy
contract C { mapping(address=>uint256) bal;
  function deposit() external payable { bal[msg.sender]+=msg.value; }
  function withdraw() external { uint256 a=bal[msg.sender]; (bool ok,)=msg.sender.call{value:a}(""); require(ok); bal[msg.sender]=0; } }
