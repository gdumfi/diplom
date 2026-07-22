// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// демо Фазы B+: УЯЗВИМЫЙ share-vault (ETH) — классическая first-depositor / share-inflation.
// нет мёртвых долей и защиты от округления: атакующий вносит 1 wei (получает 1 долю),
// донатит крупную сумму (force-send неизбежен) → цена доли раздута → депозит жертвы
// округляется в 0 долей, её ETH оседает в пуле, атакующий выкупает всё своей 1 долей.
contract ShareVault {
    mapping(address => uint256) public shares;
    uint256 public totalShares;

    // активы = ETH-баланс контракта. доли = amount * totalShares / активы_до_депозита
    function deposit() external payable returns (uint256 s) {
        uint256 assetsBefore = address(this).balance - msg.value;
        s = (totalShares == 0) ? msg.value : (msg.value * totalShares) / assetsBefore;
        shares[msg.sender] += s;
        totalShares += s;
    }

    function redeem(uint256 s) external returns (uint256 amt) {
        amt = (s * address(this).balance) / totalShares;
        shares[msg.sender] -= s;   // ревертнёт при s > доли (0.8 underflow) — норм
        totalShares -= s;
        (bool ok, ) = payable(msg.sender).call{value: amt}("");
        require(ok, "xfer");
    }

    receive() external payable {}   // допускаем донат
}
