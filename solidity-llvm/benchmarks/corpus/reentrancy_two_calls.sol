// два внешних вызова с эффектом в storage между ними -> HIGH
// (запись balance после первого вызова достижима при повторной входимости)
contract ReentrancyTwoCalls {
    uint256 balance;
    uint256 fee;
    function withdraw() public {
        require(balance > 0);
        msg.sender.call{value: balance}("");
        balance = 0;
        msg.sender.call{value: fee}("");
    }
}
