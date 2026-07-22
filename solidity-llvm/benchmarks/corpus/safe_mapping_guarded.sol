// mapping + настоящий nonReentrant guard (locked bool): запись в balances
// происходит после вызова, НО весь метод под настоящим guard -> safe/LOW
contract SafeMappingGuarded {
    bool locked;
    mapping(address => uint256) balances;
    function withdraw() public {
        require(!locked);
        locked = true;
        require(balances[msg.sender] > 0);
        msg.sender.call{value: balances[msg.sender]}("");
        balances[msg.sender] = 0;
        locked = false;
    }
}
