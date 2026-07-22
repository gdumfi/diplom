// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: arbitrary-call
// arbitrary external call (класс Multichain $126M/Socket/LiFi): низкоуровневый target.call(data),
// где target И calldata из аргументов, точка входа без проверки прав. атакующий заставляет контракт
// вызвать что угодно от своего имени → дренаж одобренных контракту токенов. blacklist НЕ спасает
contract Router {
    mapping(address => bool) blocked;
    function performAction(address target, bytes calldata data) external returns (bytes memory) {
        require(!blocked[target], "blocked");
        (bool ok, bytes memory ret) = target.call(data);
        require(ok);
        return ret;
    }
}
