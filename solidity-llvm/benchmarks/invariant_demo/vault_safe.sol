// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// демо Фазы B: БЕЗОПАСНЫЙ vault — обнуление баланса ДО внешнего вызова (CEI соблюдён).
// тот же интерфейс, что и vault_vuln. инвариант-харнесс no-overdraw должен ДЕРЖАТЬСЯ:
// реентрантный withdraw читает bal=0 → шлёт 0 → атакующий не выводит больше внесённого.
// контроль: доказывает, что свойство осмысленно (не всегда-падающее).
contract Vault {
    mapping(address => uint256) public bal;
    function deposit() external payable { bal[msg.sender] += msg.value; }
    function withdraw() external {
        uint256 a = bal[msg.sender];
        bal[msg.sender] = 0;                            // эффект ДО вызова (CEI)
        (bool ok,) = msg.sender.call{value: a}("");
        require(ok);
    }
}
