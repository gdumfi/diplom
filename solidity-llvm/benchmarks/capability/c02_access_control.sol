// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: access-control
contract C { address owner; constructor(){owner=msg.sender;}
  function setOwner(address n) external { owner=n; }
  function guarded(address n) external { require(msg.sender==owner); owner=n; } }
