// бизнес-флаг маскируется под guard: require(active) без негации — НЕ защита,
// реальная запись balance после вызова остаётся уязвимой
contract ReentrancyBusinessFlag {
    bool active;
    uint256 balance;
    function withdraw() public {
        active = true;
        require(active);
        msg.sender.call{value: balance}("");
        balance = 0;
    }
}
