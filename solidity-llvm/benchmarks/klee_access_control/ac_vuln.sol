// Уязвимый: setOwner меняет owner БЕЗ проверки прав. Любой перехватит контракт.
// CFG: HIGH access-control. KLEE: CONFIRMED (msg.sender != owner достигает записи owner).
contract Bank {
    address owner;
    uint256 config;
    function setConfig(uint256 c) public {
        require(msg.sender == owner);   // защищённая функция → выводим owner-слот (0)
        config = c;
    }
    function setOwner(address o) public {
        owner = o;                       // привилегированная запись БЕЗ проверки
    }
}
