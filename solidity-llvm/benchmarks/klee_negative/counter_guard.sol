// nonReentrant на счётчике (status==0), а не bool !flag. CFG guard НЕ распознаёт →
// ложно помечает HIGH. KLEE отвергает: реентрантный заход ревертит на require(status==0).
contract CounterGuard {
    mapping(address => uint256) balances;
    uint256 status;
    function withdraw() public {
        require(status == 0);
        status = 1;
        uint256 a = balances[msg.sender];
        msg.sender.call{value: a}("");          // effect после вызова → CFG говорит HIGH
        balances[msg.sender] = 0;
        status = 0;
    }
}
