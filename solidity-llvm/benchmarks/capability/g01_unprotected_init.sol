// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: access-control
// unprotected initializer (нет модификатора initializer): любой захватит owner (класс Nomad/прокси)
contract C { address owner;
  function initialize(address o) external { owner = o; }
  function adminSet(address o) external { require(msg.sender==owner); owner=o; } }
