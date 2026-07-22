// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: miss
// first-deposit / share-inflation: supply==0 -> sh=assets, нет минимальных/dead-shares.
// атакующий mint 1 wei + донат -> инфляция цены доли -> следующий вкладчик округляется в 0
interface IERC20 { function transferFrom(address,address,uint256) external returns(bool); function balanceOf(address) external view returns(uint256); }
contract Vault { uint256 totalShares; mapping(address=>uint256) shares; IERC20 asset;
  function deposit(uint256 assets) external returns(uint256 sh) {
    uint256 supply = totalShares;
    sh = supply == 0 ? assets : assets * supply / asset.balanceOf(address(this));
    asset.transferFrom(msg.sender, address(this), assets);
    shares[msg.sender]+=sh; totalShares+=sh; } }
