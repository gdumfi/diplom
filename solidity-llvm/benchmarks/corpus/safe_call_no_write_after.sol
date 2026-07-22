// внешний вызов есть, но записи в storage после него нет
contract SafeCallNoWrite {
    uint256 balance;
    function ping() public {
        require(balance > 0);
        msg.sender.call{value: balance}("");
    }
}
