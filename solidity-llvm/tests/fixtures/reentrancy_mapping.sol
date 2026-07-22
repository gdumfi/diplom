// per-user reentrancy (реалистичный DAO-паттерн на mapping)
contract Vault {
    mapping(address => uint256) balances;

    function deposit() public {
        balances[msg.sender] = balances[msg.sender] + msg.value;
    }

    function withdraw() public {
        require(balances[msg.sender] > 0);
        msg.sender.call{value: balances[msg.sender]}("");
        balances[msg.sender] = 0;
    }
}
