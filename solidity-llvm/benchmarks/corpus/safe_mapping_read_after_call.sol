// чтение mapping-элемента после вызова без записи -> safe
contract SafeMappingReadAfter {
    mapping(address => uint256) balances;
    function withdraw() public returns (uint256) {
        require(balances[msg.sender] > 0);
        msg.sender.call{value: balances[msg.sender]}("");
        return balances[msg.sender];
    }
}
