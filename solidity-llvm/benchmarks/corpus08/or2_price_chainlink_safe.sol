// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: none
interface IFeed { function latestAnswer() external view returns (int256); }
contract Safe1 {
    IFeed feed;
    function borrow() external {
        int256 p = feed.latestAnswer();                    // Chainlink — не DEX-spot
        (bool ok,) = msg.sender.call{value: uint256(p)}("");
        require(ok);
    }
}
