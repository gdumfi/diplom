// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// демо Фазы B: УЯЗВИМЫЙ vault — внешний вызов ДО обнуления баланса (нарушение CEI).
// инвариант-харнесс no-overdraw должен СЛОМАТЬСЯ (атакующий реентрантно крадёт TVL).
contract Vault {
    mapping(address => uint256) public bal;
    function deposit() external payable { bal[msg.sender] += msg.value; }
    function withdraw() external {
        uint256 a = bal[msg.sender];
        (bool ok,) = msg.sender.call{value: a}("");   // вызов ПЕРЕД эффектом
        require(ok);
        bal[msg.sender] = 0;                            // эффект после — окно reentrancy
    }
}
