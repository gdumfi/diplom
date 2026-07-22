// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// демо Фазы B+: БЕЗОПАСНЫЙ share-vault — мёртвые доли (dead shares) на первом депозите.
// DEAD долей навсегда сжигаются в address(0), поэтому раздуть цену доли донатом
// неэкономично: чтобы обнулить доли жертвы, атакующий должен задонатить кратно больше,
// чем украдёт, а мёртвые доли всё равно съедают его прибыль. тот же интерфейс.
contract ShareVault {
    mapping(address => uint256) public shares;
    uint256 public totalShares;
    uint256 constant DEAD = 1e6;   // мёртвые доли

    function deposit() external payable returns (uint256 s) {
        uint256 assetsBefore = address(this).balance - msg.value;
        if (totalShares == 0) {
            require(msg.value > DEAD, "bootstrap too small");
            shares[address(0)] += DEAD;         // навсегда заблокированы
            s = msg.value - DEAD;
            shares[msg.sender] += s;
            totalShares += msg.value;           // = DEAD + s
        } else {
            s = (msg.value * totalShares) / assetsBefore;
            shares[msg.sender] += s;
            totalShares += s;
        }
    }

    function redeem(uint256 s) external returns (uint256 amt) {
        amt = (s * address(this).balance) / totalShares;
        shares[msg.sender] -= s;
        totalShares -= s;
        (bool ok, ) = payable(msg.sender).call{value: amt}("");
        require(ok, "xfer");
    }

    receive() external payable {}
}
