// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;

// SampleVault — intentionally vulnerable demo target used to generate real SCSA output
// for the web console (`sca --json` / `--llm-context`). Not a real protocol.
contract SampleVault {
    address public owner;
    mapping(address => uint256) public bal;

    constructor() { owner = msg.sender; }

    modifier onlyOwner() { require(msg.sender == owner, "not owner"); _; }

    function deposit() external payable { bal[msg.sender] += msg.value; }

    // HIGH reentrancy — external call precedes the state update (CEI violation)
    function withdraw() external {
        uint256 a = bal[msg.sender];
        (bool ok, ) = msg.sender.call{value: a}("");
        require(ok, "send");
        bal[msg.sender] = 0;
    }

    // HIGH access-control — writes the owner slot with no guard (cf. safeSetOwner)
    function setOwner(address n) external { owner = n; }

    function safeSetOwner(address n) external onlyOwner { owner = n; }

    // MEDIUM — tx.origin used for authorization
    function drain(address to) external {
        require(tx.origin == owner, "auth");
        (bool ok, ) = to.call{value: address(this).balance}("");
        require(ok, "x");
    }

    // LOW — unchecked low-level call
    function poke(address t) external {
        t.call("");
    }
}
