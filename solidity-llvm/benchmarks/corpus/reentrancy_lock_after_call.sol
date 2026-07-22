// require(!locked) есть, но locked взводится (locked=true) ПОСЛЕ вызова —
// на момент вызова защиты нет, повторный вход проходит проверку -> HIGH
contract ReentrancyLockAfterCall {
    bool locked;
    uint256 balance;
    function withdraw() public {
        require(!locked);
        msg.sender.call{value: balance}("");
        locked = true;
        balance = 0;
        locked = false;
    }
}
