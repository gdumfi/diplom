// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: miss
// signature-replay: ecrecover авторизует выплату, но нет nonce/used[hash] -> повтор той же подписи
contract C { mapping(address=>uint256) bal;
  function claim(uint256 amount, uint8 v, bytes32 r, bytes32 s) external {
    bytes32 h = keccak256(abi.encodePacked(msg.sender, amount));
    address signer = ecrecover(h, v, r, s);
    require(signer != address(0));
    bal[msg.sender] += amount; } }
