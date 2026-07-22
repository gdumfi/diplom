// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: none
contract PoolSafe {
    uint256 totalValue;
    mapping(address=>uint256) shares;
    function withdraw() external {
        uint256 amt = shares[msg.sender];
        totalValue -= amt;
        (bool ok,)=msg.sender.call{value:amt}("");
        require(ok);
    }
    function virtualPrice() external view returns (uint256) { return totalValue; }
}
