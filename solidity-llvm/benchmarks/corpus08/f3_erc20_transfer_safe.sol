// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: none
// Fix8: token.transfer(recv, amt) на контракте/интерфейсе — ERC20-метод (value=0), а НЕ нативный
// address.transfer(value). раньше args[0] (получатель recv, storage-адрес) ошибочно брался как
// value → его слот попадал в value-трассу и давал ложную cross-function reentrancy (MOG clearStuckToken)
interface IERC20 { function transfer(address to, uint256 amt) external returns (bool); }
contract Sweeper {
    address recv;
    function setRecv(address r) external { recv = r; }   // пишет recv в другой функции
    function clearStuck(address token, uint256 amt) external returns (bool) {
        return IERC20(token).transfer(recv, amt);        // value=0 ERC20 — не ETH-перевод
    }
}
