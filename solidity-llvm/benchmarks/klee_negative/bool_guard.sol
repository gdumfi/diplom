// nonReentrant на bool-флаге: CFG распознаёт guard (LOW), KLEE подтверждает безопасность.
contract BoolGuard {
    mapping(address => uint256) balances;
    bool locked;
    function withdraw() public {
        require(!locked);
        locked = true;
        uint256 a = balances[msg.sender];
        msg.sender.call{value: a}("");         // effect после, но guard блокирует повтор
        balances[msg.sender] = 0;
        locked = false;
    }
}
