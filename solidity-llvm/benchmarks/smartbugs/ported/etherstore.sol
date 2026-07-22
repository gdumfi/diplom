// портировано вручную из SmartBugs dataset/reentrancy/etherstore.sol
// (EtherStore, sigp/solidity-security-blog). списание баланса после call. expected=1
contract EtherStore {
    mapping(address => uint256) balances;

    function depositFunds() public {
        balances[msg.sender] = balances[msg.sender] + msg.value;
    }

    function withdrawFunds(uint256 amount) public {
        require(balances[msg.sender] >= amount);
        // отправка эфира ДО обновления баланса -> реентранси
        msg.sender.call{value: amount}("");
        balances[msg.sender] = balances[msg.sender] - amount;
    }
}
