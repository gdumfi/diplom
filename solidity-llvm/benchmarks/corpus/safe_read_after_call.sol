// после вызова только ЧТЕНИЕ storage, без записи
contract SafeReadAfter {
    uint256 balance;
    function withdraw() public returns (uint256) {
        msg.sender.call{value: balance}("");
        return balance;
    }
}
