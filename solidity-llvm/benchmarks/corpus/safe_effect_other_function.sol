// вызов в одной функции, запись storage — в другой. детектор внутрипроцедурный,
// поэтому в withdraw после вызова записи нет -> safe (метка честная: 0).
// межпроцедурный анализ мог бы посчитать иначе, но здесь это вне области.
contract SafeEffectOtherFunction {
    uint256 balance;
    function withdraw() public {
        require(balance > 0);
        msg.sender.call{value: balance}("");
    }
    function reset() public {
        balance = 0;
    }
}
