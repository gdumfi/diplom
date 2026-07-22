# Неделя 6 на пальцах — codegen

---

## Что мы сделали

Превратили AST (дерево, которое мы строили на неделях 3-4 и обогащали на неделе 5) в **настоящий LLVM IR**. Это первый момент, когда наш компилятор реально что-то **делает**: на вход `.sol`, на выход `.ll` — текстовый файл, который понимают `llvm-as`, `opt`, `llc` и наши будущие анализаторы уязвимостей.

Пайплайн теперь полный:
```
.sol файл
  ↓ Lexer (неделя 2)
поток токенов
  ↓ Parser (недели 3-4)
AST
  ↓ Sema (неделя 5)
AST + аннотации
  ↓ CodeGen (неделя 6) ← мы здесь
LLVM IR (.ll)
```

Запусти команду:
```bash
cd ~/diplom/solidity-llvm
./build/src/solidity-llvm --input tests/fixtures/counter_no_require.sol --emit-ir
```

И увидишь настоящий LLVM IR, а не stub-заглушку.

---

## Главные термины

### LLVM IR
«Промежуточный язык» (Intermediate Representation). Он похож на ассемблер, но «универсальный» — не привязан к конкретному процессору. Любой инструмент LLVM может его читать.

Пример:
```llvm
define i256 @add(i256 %a, i256 %b) {
entry:
  %sum = add i256 %a, %b
  ret i256 %sum
}
```
Это функция, принимающая два 256-битных числа и возвращающая их сумму. Запись очень регулярная: каждая инструкция — одна строка с фиксированной формой.

### IRBuilder
Это «карандаш», которым мы пишем IR. Вместо ручной конкатенации строк, мы пишем:
```cpp
auto* sum = builder.CreateAdd(a, b, "sum");
```
И библиотека LLVM сама построит инструкцию правильно. Это удобнее и безопаснее, чем городить текст.

### Module
Контейнер для всего IR. Один Module = один файл `.ll`. Содержит функции, глобалы, объявления.

### BasicBlock
Один «непрерывный» кусок кода, который выполняется без ветвлений. Заканчивается **terminator**'ом (`ret`, `br`, `call` отдельно — нет, это не terminator). В каждой функции минимум 1 basic block — entry.

Когда у нас `if/else` — это 3 блока: then, else, merge (обычно).

### `alloca`
Инструкция LLVM, означающая «выдели мне место в стеке». Возвращает указатель. Похоже на `T* x = new T;` но автоматически освобождается при выходе из функции.

Мы используем `alloca` для каждой локальной переменной и параметра. Это стандартный clang-стиль: даже `int x = 5;` компилируется в alloca + store.

---

## Маппинг типов

| Solidity | LLVM | Почему |
|---|---|---|
| `uint256` | `i256` | LLVM умеет 256-битные integers из коробки |
| `bool` | `i1` | один бит, как и должно быть |
| `address` | `i160` | в Ethereum адрес — это 20 байт = 160 бит |

`uint256` нативно ни один процессор не умеет, но **LLVM умеет** работать с произвольной шириной integer-типов. Когда дело дойдёт до настоящей кодогенерации под x86 — `llc` выдаст ошибку, потому что i256 в железе нет. Это **ожидаемо** — наш компилятор не для запуска на x86, а для **статического анализа** (LLVM-passes). Анализаторам ширина числа не мешает.

---

## EVM-специфика — через intrinsic-функции

EVM — это «процессор» Ethereum-смартконтрактов. У него нет регистров — есть **storage** (постоянное хранилище), **memory** (временное), **stack**. Чтобы прочитать state variable `value`, EVM-байткод делает инструкцию `SLOAD slot`.

Мы не генерим EVM-байткод (это уже промышленная задача). Но мы хотим в IR **отметить**: «вот тут происходит чтение из storage». Решение — **intrinsic-функции**:

```llvm
declare i256 @evm.sload(i256 slot)
declare void @evm.sstore(i256 slot, i256 value)
```

`declare` без `define` означает «эта функция объявлена, тела у неё нет». Это **обещание** — мол, кто-то её предоставит. Для статического анализа этого достаточно: проход анализа видит `call @evm.sload(...)` и понимает: «здесь чтение storage».

Когда наши анализаторы для дипломной части будут искать reentrancy — они будут искать паттерн «`call_external` ... `evm.sstore`» — потому что это значит «мы делаем внешний вызов до записи в storage, что небезопасно».

---

## Как работает чтение state var

`value` (uint256, state var в слоте 0):

```cpp
emitStateLoad(slot=0, kind=Uint256)
```

Что эмитится:
```llvm
%sload = call i256 @evm.sload(i256 0)
```
И возвращается этот `%sload` как `Value*` — дальше его можно использовать в `add`, `ret`, etc.

Если бы `value` был `bool` (i1):
```llvm
%sload = call i256 @evm.sload(i256 0)
%trunc = trunc i256 %sload to i1
```
Сначала прочитали i256, потом обрезали до i1.

---

## Параметры функции

Когда `function f(uint256 x) public { ... }` входит на codegen:

1. Создаём `i256 (i256)` функцию.
2. Создаём entry-блок.
3. Делаем `alloca i256` для `x` (это даёт указатель на стек).
4. `store i256 %0, ptr %x` (сохраняем входящий arg в alloca).
5. Регистрируем `x → {alloca, i256}` в `locals_`.

Теперь каждое использование `x` в теле — это `load i256, ptr %x`. Эта схема — стандартный паттерн clang. Оптимизатор LLVM на `-O1+` выкосит лишние allocas через mem2reg pass. Нам это не важно — мы не оптимизируем.

---

## if / else — три блока

Для `if (cond) { ... } else { ... }`:

```llvm
entry:
  %c = <вычисление cond>
  br i1 %c, label %if.then, label %if.else

if.then:
  <код then>
  br label %if.merge

if.else:
  <код else>
  br label %if.merge

if.merge:
  <код после if>
```

`br i1 %c, label %X, label %Y` — условный прыжок: если `%c == 1`, переходим в `%X`, иначе в `%Y`.

### Тонкость: что если обе ветки `return`?

```solidity
if (x) { return 1; } else { return 2; }
```

Тогда `if.merge` никому не нужен — никто на него не прыгает. Если оставить его пустым, LLVM verifier ругнётся: «basic block without terminator». Если поставить туда `unreachable` — формально терминатор, но **builder продолжает указывать на этот блок**, и если после `if/else` идёт ещё код (даже safety-net `ret 0`), мы попытаемся вставить инструкцию **после** `unreachable` — это уже UB или ассерт LLVM.

Решение, которое реализовали:
1. Если `mergeBB->hasNPredecessors(0)` — `eraseFromParent` (удаляем блок).
2. В самом начале `emitStmt` проверка: `if (builder_->GetInsertBlock()->getTerminator()) return;` — молча пропускаем мёртвый код.

Plan-reviewer пред-обнаружил это в плане. **До** написания кода. Спасли часы отладки.

---

## Driver integration

`Driver::run` теперь на `--emit-ir` делает полный pipeline:
1. Открыть файл.
2. Lexer → Parser → SemanticAnalyzer → CodeGen.
3. На любой ошибке: распечатать диагностики (`error: line X:Y: message`) и return 1.
4. На успехе: `module->print(stdout)` или в `--output` файл.

Stub-заглушка `; solidity-llvm: stub output, source length=N` — выкинута. Теперь компилятор реально компилирует.

---

## Что теперь работает end-to-end

Контракт `counter_no_require.sol`:
```solidity
contract Counter {
    uint256 public value;

    function increment(uint256 delta) public returns (uint256) {
        uint256 newValue = value + delta;
        value = newValue;
        return newValue;
    }

    function isPositive(uint256 x) public returns (bool) {
        return x > 0;
    }
}
```

После `solidity-llvm --input ... --emit-ir`:
```llvm
declare i256 @evm.sload(i256)
declare void @evm.sstore(i256, i256)

define i256 @increment(i256 %0) {
entry:
  %delta = alloca i256, align 8
  store i256 %0, ptr %delta
  %sload = call i256 @evm.sload(i256 0)        ; читаем value из слота 0
  %delta1 = load i256, ptr %delta              ; читаем delta
  %add = add i256 %sload, %delta1              ; сумма
  %newValue = alloca i256, align 8
  store i256 %add, ptr %newValue               ; uint256 newValue = ...
  %newValue1 = load i256, ptr %newValue
  call void @evm.sstore(i256 0, i256 %newValue1) ; value = newValue
  %newValue2 = load i256, ptr %newValue
  ret i256 %newValue2                          ; return newValue
}

define i1 @isPositive(i256 %0) {
entry:
  %x = alloca i256
  store i256 %0, ptr %x
  %x1 = load i256, ptr %x
  %gt = icmp ugt i256 %x1, 0
  ret i1 %gt
}
```

И это `.ll` проходит `llvm-as` без ошибок — то есть **валидный LLVM IR**.

---

## Что НЕ работает (на следующую неделю)

Полный `counter.sol` (с `require(msg.sender == owner)`) пока **не компилируется** — codegen выдаст diagnostic на `MemberExpr` (msg.sender) и на `CallExpr` (внутри RequireStmt cond прошло, но если бы вдруг где-то ещё). На самом деле `RequireStmt` сейчас просто пропускается (default-ветка emitStmt). Это значит: если фронтенд получит require — он ничего не эмитит, и проверка молча исчезает.

На неделе 7 это всё закроем.

---

## Главные ловушки, через которые прошли

1. **LLVMContext lifetime.** Module ссылается на context, который владеет CodeGen. Если выкинуть CodeGen — Module dangle'ит. Implementer на лету заметил, и в helper `compileSrc` сделал `struct Compiled` с обеими unique_ptr.

2. **Opaque pointers (LLVM 16+).** Раньше `i256*` хранил информацию о pointee type. Теперь все указатели — просто `ptr`. `CreateLoad` требует явно: «загрузи мне такой-то тип». Поэтому в `LocalSlot` хранится и `AllocaInst*`, и `Type*`.

3. **AssignStmt не имеет resolved kind/slot.** AST хранит только имя цели, не «это state var в слоте 1». CodeGen строит `stateVarSlots_` maps один раз при входе.

4. **If/else merge block.** Решили через `hasNPredecessors(0) + eraseFromParent + guard`.

---

## Что дальше — неделя 7

Финал фронтенда. Добавим:
- 3 новых intrinsic'а: `evm.caller()`, `evm.callvalue()`, `evm.revert(offset, len)`.
- `msg.sender` → `call i160 @evm.caller()`.
- `msg.value` → `call i256 @evm.callvalue()`.
- `require(cond)` → CondBr на `ok` и `revert` блоки; в revert — `call evm.revert + unreachable`.
- Полный `counter.sol` (с require) end-to-end → `.ll` проходит `llvm-as`.

После недели 7 фронтенд **закончен**. Дальше — неделя 8 (полировка CLI + тесты + пояснительная записка) и считай курсовая в кармане.

---

## Краткое резюме

- Превращаем AST в LLVM IR через `IRBuilder`.
- `uint256→i256`, `bool→i1`, `address→i160`.
- State vars: через intrinsic'и `evm.sload`/`evm.sstore`.
- Локальные: `alloca` + `load`/`store`.
- Арифметика — нативный LLVM `add`/`udiv`/`icmp`.
- `if/else` — три BB с CondBr; мёртвый merge — eraseFromParent.
- Driver интегрирован: полный pipeline на `--emit-ir`, реальный IR на выходе.
- 93/93 тестов, Counter (без require) end-to-end → `.ll` проходит `llvm-as`.
- На неделе 7 — require + msg.* + полный counter.sol.

Это самая интересная неделя — отсюда видно, как из текста рождается реальный машинный код.
