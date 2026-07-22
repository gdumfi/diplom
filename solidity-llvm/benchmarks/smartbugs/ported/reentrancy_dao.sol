// портировано вручную из SmartBugs dataset/reentrancy/reentrancy_dao.sol
// (упрощённый TheDAO splitDAO/withdraw). expected=1
contract ReentrancyDAO {
    mapping(address => uint256) balances;

    function deposit() public {
        balances[msg.sender] = balances[msg.sender] + msg.value;
    }

    function withdraw(uint256 amount) public {
        require(balances[msg.sender] >= amount);
        // перевод перед списанием -> уязвимость TheDAO
        msg.sender.call{value: amount}("");
        balances[msg.sender] = balances[msg.sender] - amount;
    }
}
