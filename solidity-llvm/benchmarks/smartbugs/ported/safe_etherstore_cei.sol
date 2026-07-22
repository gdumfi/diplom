// безопасный аналог EtherStore: списание баланса ДО внешнего вызова (CEI).
// портировано вручную (исправленная версия SmartBugs-паттерна). expected=0
contract SafeEtherStore {
    mapping(address => uint256) balances;

    function depositFunds() public {
        balances[msg.sender] = balances[msg.sender] + msg.value;
    }

    function withdrawFunds(uint256 amount) public {
        require(balances[msg.sender] >= amount);
        // сначала обновляем состояние, потом отправляем -> безопасно
        balances[msg.sender] = balances[msg.sender] - amount;
        msg.sender.call{value: amount}("");
    }
}
