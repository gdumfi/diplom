// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: weak-randomness
contract Raffle {
    uint256 public winnerIndex; uint256 public count;
    function draw() external { winnerIndex = block.timestamp % count; }  // SWC-120
}
