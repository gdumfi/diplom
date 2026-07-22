// checks-effects-interactions соблюдён: обнуление ДО вызова
contract SafeCEI {
    uint256 balance;
    function withdraw(uint256 amt) public {
        require(balance > 0);
        balance = 0;
        msg.sender.call{value: amt}("");
    }
}
