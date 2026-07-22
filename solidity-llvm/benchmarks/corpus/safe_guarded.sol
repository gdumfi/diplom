// настоящий nonReentrant: require(!locked); locked=true; ...; locked=false
contract SafeGuarded {
    bool locked;
    uint256 balance;
    function withdraw() public {
        require(!locked);
        locked = true;
        require(balance > 0);
        msg.sender.call{value: balance}("");
        balance = 0;
        locked = false;
    }
}
