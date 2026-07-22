// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: none
contract S3 {
    mapping(address => uint256) public bal;
    bool private locked;
    modifier nonReentrant() { require(!locked); locked = true; _; locked = false; }
    function withdraw() external nonReentrant {
        uint256 a = bal[msg.sender];
        (bool ok,) = msg.sender.call{value: a}("");
        require(ok);
        bal[msg.sender] = 0;              // защищено guard'ом
    }
}
