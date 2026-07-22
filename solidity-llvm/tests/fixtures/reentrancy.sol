// классический reentrancy-паттерн (нарушение checks-effects-interactions):
// внешний вызов происходит ДО обнуления storage-баланса, поэтому атакующий
// из fallback может повторно войти в withdraw, пока balance ещё не 0.
contract Vault {
    uint256 balance;

    function deposit() public {
        balance = balance + msg.value;
    }

    function withdraw() public {
        require(balance > 0);
        msg.sender.call{value: balance}("");   // interaction: управление уходит наружу
        balance = 0;                            // effect: ПОСЛЕ вызова → уязвимость
    }
}
