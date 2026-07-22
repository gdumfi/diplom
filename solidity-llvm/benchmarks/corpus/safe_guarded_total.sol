// настоящий guard: require(!locked); locked=true; ... locked=false в конце.
// эффект в mapping под защитой, но обычным полем total тоже пишем после вызова
// — всё под guard -> safe/LOW
contract SafeGuardedTotal {
    bool locked;
    uint256 total;
    function withdraw(uint256 amt) public {
        require(!locked);
        locked = true;
        msg.sender.call{value: amt}("");
        total = total + amt;
        locked = false;
    }
}
