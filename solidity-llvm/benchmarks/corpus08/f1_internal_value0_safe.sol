// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: none
// Fix6: внутренний вызов helper, делающего value=0 внешний вызов (доверенный IRM/oracle) ПЕРЕД
// записью учёта — это НЕ ETH-drain reentrancy. highCapable-пропагация через внутренние вызовы
// (иначе КАЖДАЯ функция, зовущая accrue перед записью, ложно HIGH — как Morpho supply/borrow)
interface IIrm { function borrowRate() external returns (uint256); }
contract Market {
    uint256 totalBorrow;
    address irm;
    function accrue() internal returns (uint256) { return IIrm(irm).borrowRate(); } // external value=0
    function supply(uint256 a) external {
        uint256 r = accrue();               // внутренний вызов (вложенный внешний вызов value=0)
        totalBorrow = totalBorrow + a + r;  // запись учёта после — не drain
    }
}
