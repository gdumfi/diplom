// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// демо Фазы B+: УЯЗВИМЫЙ ERC4626-подобный vault над ERC20-активом — first-depositor/
// share-inflation. доли считаются по totalAssets ДО депозита (balanceOf), поэтому донат
// актива напрямую в пул раздувает цену доли → депозит жертвы округляется в 0 долей →
// атакующий выкупает всё своей 1 долей. это реальная форма (asset/transferFrom), а не ETH.
interface IERC20 {
    function transferFrom(address, address, uint256) external returns (bool);
    function transfer(address, uint256) external returns (bool);
    function balanceOf(address) external view returns (uint256);
}

contract Vault {
    IERC20 public asset;
    mapping(address => uint256) public shares;
    uint256 public totalShares;

    constructor(address a) { asset = IERC20(a); }

    function totalAssets() public view returns (uint256) { return asset.balanceOf(address(this)); }

    // доли = assets * totalShares / totalAssets_ДО (нет мёртвых долей — уязвимо)
    function deposit(uint256 assets) external returns (uint256 s) {
        uint256 ta = totalAssets();                       // до прихода этого депозита
        s = (totalShares == 0) ? assets : (assets * totalShares) / ta;
        require(asset.transferFrom(msg.sender, address(this), assets), "tf");
        shares[msg.sender] += s;
        totalShares += s;
    }

    function redeem(uint256 s) external returns (uint256 assets) {
        assets = (s * totalAssets()) / totalShares;
        shares[msg.sender] -= s;
        totalShares -= s;
        require(asset.transfer(msg.sender, assets), "t");
    }
}
