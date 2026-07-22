// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: none
contract OZGuarded {
    address _owner;
    function _msgSender() internal view returns (address) { return msg.sender; }
    function owner() public view returns (address) { return _owner; }
    function _checkOwner() internal view { if (owner() != _msgSender()) revert(); }
    function destroy() external { _checkOwner(); selfdestruct(payable(msg.sender)); }
}
