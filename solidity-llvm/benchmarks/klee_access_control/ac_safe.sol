// Безопасный: setOwner ЗАЩИЩЁН require(msg.sender==owner). KLEE: CLEAN (путь к записи
// требует msg.sender==owner → оракул non-owner недостижим). Форс: --klee-entry setOwner
// --klee-slot 0 --klee-owner-slot 0 (детектор его не флагует — функция проверена).
contract Bank {
    address owner;
    uint256 config;
    function setConfig(uint256 c) public {
        require(msg.sender == owner);
        config = c;
    }
    function setOwner(address o) public {
        require(msg.sender == owner);
        owner = o;
    }
}
