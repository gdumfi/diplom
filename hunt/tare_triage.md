# Tare triage (17:19)

- Loans.sol: clean
## LoansExchange.sol — HAS FINDINGS
```json
[
  {
    "rule": "access-control",
    "severity": "HIGH",
    "function": "createOffer",
    "line": 80,
    "storageSlot": 3,
    "symConfirmed": false,
    "priorityScore": 350,
    "message": "функция меняет привилегированное состояние (слот 3) без проверки прав (в контракте есть проверка msg.sender==owner / admins[msg.sender] в других функциях, здесь её нет): отсутствие контроля доступа"
  }
]
```
- LoansLedger.sol: clean
- LoansNFT.sol: clean
- NavCalculator.sol: clean
## PortfolioVault.sol — HAS FINDINGS
```json
[
  {
    "rule": "access-control",
    "severity": "HIGH",
    "function": "updateNav",
    "line": 315,
    "storageSlot": 11,
    "symConfirmed": false,
    "priorityScore": 350,
    "message": "функция меняет привилегированное состояние (слот 11) без проверки прав (в контракте есть проверка msg.sender==owner / admins[msg.sender] в других функциях, здесь её нет): отсутствие контроля доступа"
  },
  {
    "rule": "access-control",
    "severity": "HIGH",
    "function": "requestDeposit",
    "line": 615,
    "storageSlot": 17,
    "symConfirmed": false,
    "priorityScore": 350,
    "message": "функция меняет привилегированное состояние (слот 17) без проверки прав (в контракте есть проверка msg.sender==owner / admins[msg.sender] в других функциях, здесь её нет): отсутствие контроля доступа"
  },
  {
    "rule": "access-control",
    "severity": "HIGH",
    "function": "cancelDepositRequest",
    "line": 677,
    "storageSlot": 17,
    "symConfirmed": false,
    "priorityScore": 350,
    "message": "функция меняет привилегированное состояние (слот 17) без проверки прав (в контракте есть проверка msg.sender==owner / admins[msg.sender] в других функциях, здесь её нет): отсутствие контроля доступа"
  }
]
```
- SmartAccountFactory.sol: clean
- TrustedCalls.sol: clean
- TrustedSpender.sol: clean
- VaultShareToken.sol: clean
- misc/GuardianAccessControl.sol: clean
- misc/LoansAuth.sol: clean
- misc/Rescuable.sol: clean

итого: 13 файлов, 2 с находками
