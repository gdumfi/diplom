// mapping-эффект после вызова спрятан в ветке if — всё равно достижим -> HIGH
contract ReentrancyMappingInIf {
    mapping(address => uint256) balances;
    function withdraw(uint256 amt) public {
        require(balances[msg.sender] > 0);
        msg.sender.call{value: amt}("");
        if (amt > 0) { balances[msg.sender] = 0; }
    }
}
