// запись storage ДО внешнего вызова (эффект раньше interaction)
contract SafeWriteBefore {
    uint256 balance;
    function deposit() public {
        balance = balance + msg.value;
        msg.sender.call{value: balance}("");
    }
}
