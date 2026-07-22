// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// демо Фазы B+: БЕЗОПАСНОЕ округление — convertToShares округляет ВНИЗ (floor, в пользу
// пула). round-trip не создаёт стоимости: convertToAssets(convertToShares(x)) <= x всегда.
// тот же интерфейс, что и erc4626_round_vuln.
interface IERC20 {
    function transferFrom(address, address, uint256) external returns (bool);
    function balanceOf(address) external view returns (uint256);
}

contract Vault {
    IERC20 public asset;
    uint256 public totalShares;

    constructor(address a) { asset = IERC20(a); }

    function totalAssets() public view returns (uint256) { return asset.balanceOf(address(this)); }

    // безопасно: floor — доли округляются ВНИЗ (в пользу пула)
    function convertToShares(uint256 assets) public view returns (uint256) {
        uint256 ta = totalAssets();
        if (totalShares == 0 || ta == 0) return assets;
        return (assets * totalShares) / ta;              // округление вниз
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
