// require(!paused) есть, но флаг НЕ взводится (нет locked=true) — это не lock,
// значит вызов не защищён, запись после него уязвима
contract ReentrancyCheckNoSet {
    bool paused;
    uint256 balance;
    function withdraw() public {
        require(!paused);
        msg.sender.call{value: balance}("");
        balance = 0;
    }
}
