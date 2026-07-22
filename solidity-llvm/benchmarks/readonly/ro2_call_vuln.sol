// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// read-only reentrancy, .call(data) триггер с value 0 (токен-хук / произвольный callback).
// СЕЙЧАС ПРОПУСКАЕТСЯ (детектор требует value>0). Должно ловиться после фикса.
contract Pool {
    uint256 public totalShares;
    function sync(address cb) external {
        (bool ok, ) = cb.call("");          // value 0 — но контроль уходит наружу
        require(ok);
        totalShares += 1;                   // мутация ПОСЛЕ вызова
    }
    function price() external view returns (uint256) { return totalShares; }
}
