// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// демо Фазы B+: БЕЗОПАСНЫЙ ERC4626-подобный vault — мёртвые доли на первом депозите.
// DEAD долей навсегда сжигаются в address(0): раздуть цену доли донатом неэкономично,
// депозит жертвы не украсть. тот же интерфейс, что и erc4626_vuln.
interface IERC20 {
    function transferFrom(address, address, uint256) external returns (bool);
    function transfer(address, uint256) external returns (bool);
    function balanceOf(address) external view returns (uint256);
}

contract Vault {
    IERC20 public asset;
    mapping(address => uint256) public shares;
    uint256 public totalShares;
    uint256 constant DEAD = 1e6;

    constructor(address a) { asset = IERC20(a); }

    function totalAssets() public view returns (uint256) { return asset.balanceOf(address(this)); }

    function deposit(uint256 assets) external returns (uint256 s) {
        uint256 ta = totalAssets();
        if (totalShares == 0) {
            require(assets > DEAD, "bootstrap too small");
            shares[address(0)] += DEAD;          // навсегда заблокированы
            s = assets - DEAD;
            require(asset.transferFrom(msg.sender, address(this), assets), "tf");
            shares[msg.sender] += s;
            totalShares += assets;               // = DEAD + s
        } else {
            s = (assets * totalShares) / ta;
            require(asset.transferFrom(msg.sender, address(this), assets), "tf");
            shares[msg.sender] += s;
            totalShares += s;
        }
    }

    function redeem(uint256 s) external returns (uint256 assets) {
        assets = (s * totalAssets()) / totalShares;
        shares[msg.sender] -= s;
        totalShares -= s;
        require(asset.transfer(msg.sender, assets), "t");
    }
}
