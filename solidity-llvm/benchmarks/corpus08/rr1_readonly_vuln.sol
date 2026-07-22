// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: read-only-reentrancy
contract Pool {
    uint256 totalValue;
    mapping(address=>uint256) shares;
    bool locked;
    modifier nonReentrant(){ require(!locked); locked=true; _; locked=false; }
    function withdraw() external nonReentrant {
        uint256 amt = shares[msg.sender];
        (bool ok,)=msg.sender.call{value:amt}("");
        require(ok);
        totalValue -= amt;
    }
    function virtualPrice() external view returns (uint256) { return totalValue; }
}
