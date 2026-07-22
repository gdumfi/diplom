// классика: внешний вызов до обнуления баланса
contract ReentrancyClassic {
    uint256 balance;
    function withdraw() public {
        require(balance > 0);
        msg.sender.call{value: balance}("");
        balance = 0;
    }
}
