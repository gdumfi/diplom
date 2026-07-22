// безопасный аналог Reentrance: обнуление до вызова (CEI). expected=0
contract SafeReentrance {
    mapping(address => uint256) userBalance;

    function addToBalance() public {
        userBalance[msg.sender] = userBalance[msg.sender] + msg.value;
    }

    function withdrawBalance() public {
        // обнуляем баланс ПЕРЕД отправкой -> безопасно
        userBalance[msg.sender] = 0;
        msg.sender.call{value: userBalance[msg.sender]}("");
    }
}
