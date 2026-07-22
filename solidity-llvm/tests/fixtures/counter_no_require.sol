contract Counter {
    uint256 public value;

    function increment(uint256 delta) public returns (uint256) {
        uint256 newValue = value + delta;
        value = newValue;
        return newValue;
    }

    function isPositive(uint256 x) public returns (bool) {
        return x > 0;
    }
}
