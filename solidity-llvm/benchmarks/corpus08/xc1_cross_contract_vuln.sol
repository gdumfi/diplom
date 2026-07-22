// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: reentrancy
interface ILedger { function debit(address u, uint256 a) external; }
contract Ledger is ILedger {
    mapping(address=>uint256) bal;
    function debit(address u, uint256 a) external { bal[u] -= a; }
}
contract Vault {
    ILedger ledger;
    function withdraw(uint256 a) external {
        (bool ok,) = msg.sender.call{value: a}("");   // ETH-value триггер
        require(ok);
        ledger.debit(msg.sender, a);                   // эффект в ДРУГОМ контракте после
    }
}
