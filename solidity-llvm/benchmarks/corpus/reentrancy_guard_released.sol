// guard снят ДО внешнего вызова (locked=false перед call) — на момент вызова
// защиты нет, значит уязвимость реальна. детектор НЕ должен считать это guarded
contract ReentrancyGuardReleased {
    bool locked;
    uint256 balance;
    function withdraw() public {
        require(!locked);
        locked = true;
        locked = false;
        msg.sender.call{value: balance}("");
        balance = 0;
    }
}
