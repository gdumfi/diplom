contract Counter {
    uint256 public value;
    address public owner;

    function increment(uint256 delta) public returns (uint256) {
        require(msg.sender == owner);
        uint256 newValue = value + delta;
        value = newValue;
        return newValue;
    }

    function isOwner(address who) public returns (bool) {
        return who == owner;
    }
}
