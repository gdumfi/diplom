// тот же порядок «вызов -> запись storage», что и в reentrancy.sol, но функция
// защищена nonReentrant-guard: require(!locked); locked = true; ... call ...
// детектор обязан распознать guard и НЕ выдавать HIGH (ожидаем пусто/LOW-guarded).
contract Vault {
    bool locked;
    uint256 balance;

    function withdraw() public {
        require(!locked);        // check: флаг не взведён
        locked = true;           // effect: взводим guard ДО внешнего вызова
        require(balance > 0);
        msg.sender.call{value: balance}("");  // interaction
        balance = 0;             // запись эффекта — но под защитой guard-а
        locked = false;          // release guard-а (запись в слот флага)
    }
}
