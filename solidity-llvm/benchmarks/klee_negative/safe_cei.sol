// CEI-безопасный: обнуление баланса ДО внешнего вызова. Reentrancy невозможна.
contract SafeCEI {
    mapping(address => uint256) balances;
    function withdraw() public {
        uint256 a = balances[msg.sender];
        require(a > 0);
        balances[msg.sender] = 0;              // effect ДО вызова
        msg.sender.call{value: a}("");
    }
}
