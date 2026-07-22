// несколько эффектов после вызова
contract ReentrancyPartial {
    uint256 balance;
    uint256 total;
    function withdraw() public {
        require(balance > 0);
        msg.sender.call{value: balance}("");
        balance = 0;
        total = 0;
    }
}
