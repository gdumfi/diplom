// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: arbitrary-call
// arbitrary external call: target И calldata от юзера -> дренаж approvals контракта (Multichain/Socket).
// ПОКРЫТО ArbitraryCallDetector (2026-07-12): низкоуровневый .call(data), target из аргумента, без гарда
contract C { function execute(address target, bytes calldata data) external { (bool ok,)=target.call(data); require(ok); } }
