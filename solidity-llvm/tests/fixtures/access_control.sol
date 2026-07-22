// контроль доступа: setConfigAdmin проверяет owner, а setConfig и takeOwner — нет.
// понятие owner выводится из проверки msg.sender == owner в setConfigAdmin.
contract Bank {
    address owner;
    uint256 config;

    function setConfigAdmin(uint256 c) public {
        require(msg.sender == owner);
        config = c;
    }

    function setConfig(uint256 c) public {   // привилегированный слот config без проверки
        config = c;
    }

    function takeOwner(address o) public {   // перехват владельца без проверки
        owner = o;
    }
}
