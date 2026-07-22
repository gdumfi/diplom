// mapping, CEI-корректный: обнуление баланса до внешнего вызова
contract SafeMappingCEI {
    mapping(address => uint256) balances;
    function withdraw(uint256 amt) public {
        require(balances[msg.sender] > 0);
        balances[msg.sender] = 0;
        msg.sender.call{value: amt}("");
    }
}
