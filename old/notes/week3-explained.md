# Неделя 3 на пальцах — парсер объявлений

---

## Что такое парсер

Лексер на прошлой неделе превратил текст в **поток токенов**:
```
[KwContract] [Identifier "Counter"] [LBrace]
  [KwUint256] [KwPublic] [Identifier "value"] [Semicolon]
  ...
[RBrace]
```

Но плоский список — это всё ещё «слова без понимания структуры». Парсер делает следующий шаг: **складывает их в дерево**, где видно, кто кому подчиняется.

Дерево называется **AST** — Abstract Syntax Tree, абстрактное синтаксическое дерево. Для нашего `Counter`:

```
ContractDecl "Counter"
├── stateVars:
│   ├── StateVarDecl { type=Uint256, name="value", public=true }
│   └── StateVarDecl { type=Address, name="owner", public=true }
└── functions:
    ├── FunctionDecl "increment"
    │   ├── params: [Param { type=Uint256, name="delta" }]
    │   ├── returns: Uint256
    │   └── body: { ... }      ← пока внутрь не лезем
    └── FunctionDecl "isOwner"
        ├── params: [Param { type=Address, name="who" }]
        ├── returns: Bool
        └── body: { ... }
```

Это дерево гораздо удобнее анализировать дальше: семантический анализатор (неделя 5) пробежит по нему и проверит типы, кодогенератор (неделя 6-7) превратит его в LLVM IR.

---

## Что мы сделали на этой неделе

### Скелет дерева (AST)

В файле `AST.h` описали **формы узлов** дерева. Каждый узел — это `struct`, простая структура с полями:

```cpp
struct StateVarDecl {
    Type        type;        // например, Uint256
    std::string name;        // "value"
    bool        isPublic;    // public или нет
    SourceLoc   loc;         // где в файле описана — для ошибок
};
```

Никаких наследований, виртуальных методов — просто пакеты данных. Это упрощает жизнь и нам, и компилятору C++ (быстрее работает).

Главная структура — `ContractDecl`, в ней:
- `stateVars` — массив переменных контракта
- `functions` — массив функций
- `name` — имя контракта

Каждая `FunctionDecl` хранит имя, параметры, тип возврата, и `Block body` (тело функции). На этой неделе `Block` пустой — мы про него только знаем, что он есть, но внутрь не лезем.

### Сам парсер

Класс `Parser` берёт лексер и DiagnosticEngine. Главный метод:
```cpp
std::optional<ContractDecl> parseContract();
```
Если всё хорошо — вернёт готовое дерево. Если плохо — `nullopt` и ошибки в DiagnosticEngine.

Внутри он работает **рекурсивным спуском**. Это значит: для каждой грамматической конструкции есть отдельная функция, которая её парсит и при необходимости вызывает другие функции:

```
parseContract            (видит "contract Counter { ... }")
  ├── parseStateVarDecl  (видит "uint256 public value;")
  │   └── parseType      (видит "uint256")
  ├── parseStateVarDecl  (видит "address public owner;")
  │   └── parseType      (видит "address")
  ├── parseFunctionDecl  (видит "function increment(...) public ... { ... }")
  │   ├── parseParam     (видит "uint256 delta")
  │   │   └── parseType
  │   └── parseType      (для returns)
  └── parseFunctionDecl  ("function isOwner(...) ...")
```

Каждая функция знает, **что она ждёт** на входе. Например, `parseStateVarDecl` ожидает: сначала тип, потом необязательное `public`, потом имя, потом `;`. Если что-то не так — пишет ошибку в DiagnosticEngine.

---

## Маленькие хитрости

### Lookahead и `current_`

Парсер всё время держит в руке **один следующий токен** — `current_`. Это позволяет «подглядеть» что там, не потребляя:
- `check(KwFunction)` — это функция или state var?
- Если функция — потребляем `function` и идём в `parseFunctionDecl`.
- Если нет — идём в `parseStateVarDecl`.

Только когда мы уверены, что хотим этот токен, мы вызываем `advance()` и переходим к следующему.

### `expect(TokenKind, ctx)`

Это полезный хелпер: «я **жду** вот такой токен прямо сейчас. Если он там — отлично, потребляю. Если нет — пишу ошибку в стиле:
> expected `;` in state variable declaration, got `KwReturn`».

Внутри парсера много вызовов `expect(...)` — это структурированный способ объявить «вот тут должна быть такая штука».

### Skip-тела через подсчёт скобок

В этой неделе мы решили: тело функции пока не разбираем (это неделя 4). Но просто игнорировать его нельзя — нужно понимать, где оно кончается. Иначе после функции `increment` мы окажемся посреди `value = newValue;` и совсем запутаемся.

Решение — счётчик глубины:
```cpp
int depth = 0;
for (;;) {
    if (LBrace) depth++;
    if (RBrace) {
        depth--;
        if (depth == 0) break;     // вышли из тела
    }
    advance();
}
```
Так мы корректно проскакиваем через тело любой формы — даже если внутри есть вложенные `if { } else { }`. Этот трюк называется **brace balancing**.

### Error recovery

Что если в коде есть **ошибка**? Не паниковать и падать после первой — компилятор должен по возможности сообщить **все** ошибки сразу.

Мы делаем простой recovery:
- Сломан state var → пропустить токены до ближайшей `;` или `}`, потом продолжить парсить.
- Сломана функция → пропустить тело (или до `;`/`}`), потом продолжить.

Это даёт пользователю шанс увидеть «ошибка в строке 5, и ещё одна в строке 12» вместо «ошибка в строке 5» и тишина.

В тесте `RecoversAfterBadStateVar` мы кормим:
```solidity
contract X {
    uint256 ;            // ошибка: пропущено имя
    address owner;       // нормальный state var
}
```
И ожидаем, что в результате:
- В DiagnosticEngine есть ошибка про пропущенное имя.
- `owner` всё равно попал в `stateVars` (recovery подобрал хвост).

Тест зелёный — значит recovery работает.

---

## Что теперь компилятор реально умеет

Можно прогнать на любом простом контракте:
```bash
cd ~/diplom/solidity-llvm/build
ctest --output-on-failure
```
И увидеть 28/28 тестов зелёных. Один из них — `Parser.CounterFixtureFullDeclarations` — буквально берёт наш `counter.sol` и проверяет, что:
- Контракт называется `Counter`.
- Есть `value: uint256 public` и `owner: address public`.
- Есть `increment(uint256 delta) public returns (uint256)` и `isOwner(address who) public returns (bool)`.
- Никаких ошибок в DiagnosticEngine.

Тела функций при этом ещё не разобраны — `require(msg.sender == owner)` пока для нас как пыль внутри `skipBraceBody`. Но «скелет» контракта мы понимаем правильно.

---

## Пример: что внутри после парсинга

Вход:
```solidity
contract Counter {
    uint256 public value;
    address public owner;
    function increment(uint256 delta) public returns (uint256) { ... }
}
```

После парсинга `c = p.parseContract()`:
```
c->name == "Counter"

c->stateVars[0].name == "value"
c->stateVars[0].type.kind == TypeKind::Uint256
c->stateVars[0].isPublic == true

c->stateVars[1].name == "owner"
c->stateVars[1].type.kind == TypeKind::Address
c->stateVars[1].isPublic == true

c->functions[0].name == "increment"
c->functions[0].params.size() == 1
c->functions[0].params[0].name == "delta"
c->functions[0].params[0].type.kind == TypeKind::Uint256
c->functions[0].returnType.has_value() == true
c->functions[0].returnType->kind == TypeKind::Uint256
```

Это уже **структурированные данные**, с которыми работать в разы удобнее, чем с потоком токенов.

---

## Что дальше — неделя 4

Заполняем `Block` реально. Парсер тела функции будет разбирать:
- `if (cond) { ... } else { ... }`
- `return expr;`
- `require(cond);`
- `uint256 x = expr;` (объявление локальной)
- `x = expr;` (присваивание)
- Выражения с правильным приоритетом: `1 + 2 * 3` → `(1 + (2 * 3))`, `a == b && c` → `((a == b) && c)`

Это самая большая по объёму неделя в плане — ~15 тестов. После неё AST `Counter.sol` будет полным, и парсер будет считаться готовым.

---

## Краткое резюме

- AST — дерево, описывающее структуру кода. POD-структуры, owning через `std::string`/`vector`/`optional`.
- Парсер — рекурсивный спуск, 1 токен lookahead, ошибки через DiagnosticEngine.
- На этой неделе: `contract`, state vars, заголовки функций. Тело — skipBraceBody (потом разберём).
- Базовый error recovery: пропуск до `;`/`}` при ошибке, продолжение со следующего члена.
- 28/28 тестов зелёные, включая полный `counter.sol`.
- На неделе 4 — содержимое функций (statements + expressions).

Половина курсовой осталась.
