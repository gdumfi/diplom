// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// БЕЗОПАСНО: эффект ДО вызова (CEI); getter читает не-мутируемый-после-вызова слот.
// Должно оставаться чисто (контроль precision).
contract Pool {
    uint256 public totalShares;
    uint256 public config;
    function removeLiquidity(uint256 amt) external {
        totalShares -= amt;                 // эффект ДО вызова
        (bool ok, ) = msg.sender.call{value: amt}("");
        require(ok);
    }
    function price() external view returns (uint256) { return totalShares; }
    function getConfig() external view returns (uint256) { return config; }
}
