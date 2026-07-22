// запись storage после вызова спрятана в ветке if — всё равно достижима
contract ReentrancyInIf {
    uint256 balance;
    function withdraw(uint256 amt) public {
        require(balance > 0);
        msg.sender.call{value: amt}("");
        if (amt > 0) { balance = 0; }
    }
}
