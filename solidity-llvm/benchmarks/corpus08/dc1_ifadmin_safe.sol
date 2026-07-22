// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: none
// Проход 4a: delegatecall под admin-гейтом (proxy `ifAdmin`) — target приходит от админа,
// не attacker-controlled. delegatecall достижим ТОЛЬКО при msg.sender==admin (control-dependent) →
// стандартный upgrade-паттерн, не баг (иначе FP на КАЖДОМ прокси — как было на Aave upgradeToAndCall)
contract Proxy {
    address admin;
    address impl;
    constructor() { admin = msg.sender; }
    function upgradeToAndCall(address newImpl, bytes calldata data) external {
        if (msg.sender == admin) {
            impl = newImpl;
            (bool ok, ) = newImpl.delegatecall(data);
            require(ok);
        } else {
            revert();
        }
    }
}
