// портировано вручную из SmartBugs dataset/reentrancy/reentrance.sol
// (классический Reentrance от ConsenSys). expected=1
contract Reentrance {
    mapping(address => uint256) userBalance;

    function addToBalance() public {
        userBalance[msg.sender] = userBalance[msg.sender] + msg.value;
    }

    function withdrawBalance() public {
        // вызов возвращает эфир пользователю, обнуление идёт после -> реентранси
        msg.sender.call{value: userBalance[msg.sender]}("");
        userBalance[msg.sender] = 0;
    }
}
