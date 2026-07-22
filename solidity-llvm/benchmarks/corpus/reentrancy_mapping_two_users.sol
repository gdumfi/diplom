// вызов, затем запись в mapping по КЛЮЧУ-аргументу (не msg.sender) -> HIGH
// эффект после interaction всё равно нарушает CEI
contract ReentrancyMappingTwoUsers {
    mapping(address => uint256) balances;
    function payout(address to) public {
        require(balances[to] > 0);
        to.call{value: balances[to]}("");
        balances[to] = 0;
    }
}
