// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: none
// arbitrary call ПОД контролем прав (msg.sender==owner): target приходит от доверенного админа,
// не attacker-controlled → не баг. arbitrary-call не должен флагать guarded-функции (контроль FP)
contract AdminExec {
    address owner;
    constructor() { owner = msg.sender; }
    function exec(address target, bytes calldata data) external returns (bytes memory) {
        require(msg.sender == owner, "not owner");
        (bool ok, bytes memory ret) = target.call(data);
        require(ok);
        return ret;
    }
}
