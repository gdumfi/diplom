// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: none
interface IOracle { function getPrice() external view returns (uint256); }
contract SafeV {
    IOracle oracle;
    function withdraw(uint256 a) external {
        (bool ok,) = msg.sender.call{value: a}("");   // один внешний вызов
        require(ok);
        uint256 p = oracle.getPrice();                 // view (staticcall) — не эффект
        p;
    }
}
