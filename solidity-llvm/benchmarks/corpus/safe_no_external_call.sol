// внешних вызовов нет вовсе
contract SafeNoCall {
    uint256 value;
    function increment(uint256 delta) public returns (uint256) {
        uint256 n = value + delta;
        value = n;
        return n;
    }
}
