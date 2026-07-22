// портировано вручную из SmartBugs dataset/reentrancy/simple_dao.sol
// (SimpleDAO, blockchain.unica.it). суть уязвимости сохранена:
// внешний вызов до обнуления кредита -> classic reentrancy. expected=1
contract SimpleDAO {
    mapping(address => uint256) credit;

    function donate(address to) public {
        credit[to] = credit[to] + msg.value;
    }

    function withdraw(uint256 amount) public {
        require(credit[msg.sender] >= amount);
        // внешний вызов ДО списания -> реентранси
        msg.sender.call{value: amount}("");
        credit[msg.sender] = credit[msg.sender] - amount;
    }
}
