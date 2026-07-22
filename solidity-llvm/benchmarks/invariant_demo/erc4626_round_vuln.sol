// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// демо Фазы B+: УЯЗВИМОЕ округление в ERC4626 — convertToShares округляет ВВЕРХ (ceil),
// хотя должно вниз (в пользу пула). при раздутом курсе round-trip создаёт стоимость:
// convertToAssets(convertToShares(x)) > x — из x активов «делаются» доли и обратно БОЛЬШЕ x.
// класс Balancer-2025 ($100M на направлении округления).
interface IERC20 {
    function transferFrom(address, address, uint256) external returns (bool);
    function balanceOf(address) external view returns (uint256);
}

contract Vault {
    IERC20 public asset;
    uint256 public totalShares;

    constructor(address a) { asset = IERC20(a); }

    function totalAssets() public view returns (uint256) { return asset.balanceOf(address(this)); }

    // УЯЗВИМО: ceil-деление — доли округляются ВВЕРХ в пользу пользователя
    function convertToShares(uint256 assets) public view returns (uint256) {
        uint256 ta = totalAssets();
        if (totalShares == 0 || ta == 0) return assets;
        return (assets * totalShares + ta - 1) / ta;   // округление вверх (баг)
    }

    function convertToAssets(uint256 sh) public view returns (uint256) {
        if (totalShares == 0) return sh;
        return (sh * totalAssets()) / totalShares;       // floor
    }

    function deposit(uint256 assets) external returns (uint256 s) {
        s = convertToShares(assets);
        require(asset.transferFrom(msg.sender, address(this), assets), "tf");
        totalShares += s;
    }
}
