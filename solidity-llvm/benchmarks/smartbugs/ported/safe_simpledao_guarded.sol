// безопасный аналог SimpleDAO: настоящий nonReentrant guard (locked bool).
// портировано вручную. expected=0
contract SafeSimpleDAO {
    bool locked;
    mapping(address => uint256) credit;

    function donate(address to) public {
        credit[to] = credit[to] + msg.value;
    }

    function withdraw(uint256 amount) public {
        require(!locked);
        locked = true;
        require(credit[msg.sender] >= amount);
        msg.sender.call{value: amount}("");
        credit[msg.sender] = credit[msg.sender] - amount;
        locked = false;
    }
}
