// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: delegatecall
contract C { function exec(address t, bytes calldata d) external { (bool ok,)=t.delegatecall(d); require(ok); } }
