# Вопросы преподавателя с ответами

Технические вопросы по коду компилятора с разбором. Читать с открытым
[code-walkthrough.md](code-walkthrough.md).

---

## Раздел 1. Лексер

### В1. Почему лексер написан вручную, а не через flex/bison?

**Ответ.** Грамматика подмножества небольшая — 13 ключевых слов и ~20 операторов,
ручной FSM в ~160 строк читаемее и проще отлаживается. Кроме того, ручной лексер
даёт **полный контроль над сообщениями об ошибках** — генератор обычно выдаёт
общие диагностики. По этой же причине clang/rustc используют ручные лексеры,
несмотря на то что генераторов кода у них хватает.

---

### В2. Что такое `std::string_view` и почему вы его используете в Token?

**Ответ.** `string_view` — это **не владеющий** указатель в строку: пара
`{указатель, длина}`. Это zero-cost ссылка в исходник.

```cpp
struct Token {
    TokenKind kind;
    std::string_view lexeme;   // ссылка в src_, без копирования
    SourceLoc loc;
};
```

Текст файла живёт всю компиляцию в одной `std::string` в `Driver`. Все
лексемы — это «срезы» этой строки. Без `string_view` мне пришлось бы
копировать каждую лексему в `std::string` — это O(N) аллокаций для файла из
N токенов. На большом проекте это бы убило производительность.

**Гитча.** Я **храню** лексему как `string_view`, но в AST (например,
`IdentExpr::name`) — уже как `std::string`. Почему? Потому что AST живёт после
смерти Token-объектов, и хранить ссылку на чужой буфер опасно. Перевожу
`string_view → string` в момент создания AST-узла.

---

### В3. Покажите, что выдаст лексер на строке `uint256 x = 5;`.

**Ответ.**
```
[KwUint256  "uint256"  1:1]
[Identifier "x"        1:9]
[Eq         "="        1:11]
[Number     "5"        1:13]
[Semicolon  ";"        1:14]
[EndOfFile             1:15]
```

Каждый токен — это `TokenKind` + текст лексемы + `SourceLoc{line, col}`. Текст —
это `string_view` в исходник.

---

### В4. Почему `007` запрещено, а `0` разрешено?

**Ответ.** Это требование Solidity 0.5+: leading zero в десятичных литералах
запрещено. Раньше `007` парсилось как восьмеричное (как в C) и было источником
багов. Я зеркалю это правило в [Lexer.cpp:152-154](../solidity-llvm/src/lexer/Lexer.cpp#L152-L154):

```cpp
if (lex.size() > 1 && lex[0] == '0') {
    diag_.error(startLoc, "number literal with leading zero is not allowed");
}
```

`0` (длина 1) — это валидный ноль. `007` (длина 3, начинается с 0) — ошибка.
Лексер всё равно вернёт `Number` токен (мы не падаем), просто в Diagnostic
будет ошибка — это типичный паттерн «recovery в лексере».

---

### В5. Что произойдёт на одиночном `&`?

**Ответ.** В [Lexer.cpp:108-110](../solidity-llvm/src/lexer/Lexer.cpp#L108-L110):
```cpp
case '&':
    if (match('&')) return makeToken(TokenKind::AndAnd, ...);
    break;
```
Если за `&` не идёт второго `&`, `match` вернёт `false`, мы вывалимся из
switch, и попадём в общий fallback:
```cpp
diag_.error(startLoc, std::string("unexpected char '") + c + "'");
return makeToken(TokenKind::Invalid, ...);
```
В диагностики добавляется ошибка `unexpected char '&'`, возвращается токен
`Invalid`. Парсер увидит `Invalid` и тоже среагирует ошибкой.

---

## Раздел 2. AST

### В6. Почему AST — это полиморфные классы с `virtual ~`, а не `std::variant`?

**Ответ.** Можно было бы сделать через `variant<NumberLit, BoolLit, Ident, ...>`
и `std::visit`. Это даже эффективнее по памяти (нет vtable-указателя).

Я выбрал классы с наследованием по двум причинам:
1. **Расширяемость.** На дипломе добавится 10+ новых типов узлов — добавить
   класс проще, чем расширять variant и обновлять все visitor’ы.
2. **Читаемость.** `static_cast<BinaryExpr&>(e)` после проверки `e.kind ==
   ExprKind::Binary` понятен любому, кто знает паттерн tagged union в OOP.
   `std::visit` с лямбдами — это эффективно, но менее очевидно.

Это инженерный trade-off. clang тоже использует наследование для AST.

---

### В7. Зачем поле `std::optional<Type> resolvedType` в `Expr`?

**Ответ.** Парсер выражения тип не знает — он строит дерево по форме. Тип
определяется на **семантике**, после разрешения идентификаторов и проверки
правил.

`resolvedType` — это **аннотация**, которую дописывает Sema поверх AST. До
Sema это `nullopt`. После Sema — у каждого выражения есть конкретный тип, и
CodeGen может на него опираться без повторного анализа.

Это классический паттерн **аннотированного AST**, как в clang и rustc.

---

### В8. Что такое `IdentExpr::resolvedKind` и `resolvedSlot`?

**Ответ.** В `IdentExpr` есть имя (`std::string name`), но Sema понимает, на что
оно ссылается, и записывает это:

```cpp
enum class Resolution { Unresolved, StateVar, Param, Local, Function };
Resolution resolvedKind = Resolution::Unresolved;
int        resolvedSlot = -1;   // storageSlot для StateVar, иначе -1
```

CodeGen смотрит `resolvedKind` и выбирает стратегию:
- `Local`/`Param` → `load` из соответствующей `alloca`
- `StateVar` → `call @evm.sload(i256 slot)`
- остальное — пока не поддерживается

`resolvedSlot` нужен для `StateVar` — это аргумент `sload`/`sstore`. Заполняется
Семой на этапе `registerContractMembers` (slot 0, 1, 2, …).

---

## Раздел 3. Парсер

### В9. Что такое precedence climbing? Покажите на `1 + 2 * 3`.

**Ответ.** Это алгоритм рекурсивного спуска для выражений, где приоритет
оператора передаётся через параметр `minPrec`:

```cpp
ExprPtr parseBinary(int minPrec) {
    auto lhs = parseUnary();
    while (prec(peek()) >= minPrec) {
        op = peek(); advance();
        auto rhs = parseBinary(prec(op) + 1);   // +1 = левая ассоциативность
        lhs = Binary(op, lhs, rhs);
    }
    return lhs;
}
```

Трассировка `1 + 2 * 3`:

1. `parseBinary(1)`:
   - `lhs = 1` (NumberLit)
   - `peek = '+', prec = 5 ≥ 1` — OK
   - потребляем `+`, рекурсивно `parseBinary(6)`
2. **Внутри** `parseBinary(6)`:
   - `lhs = 2`
   - `peek = '*', prec = 6 ≥ 6` — OK
   - потребляем `*`, рекурсивно `parseBinary(7)`
3. **Внутри** `parseBinary(7)`:
   - `lhs = 3`
   - `peek = EOF, prec = 0 < 7` — выходим, возвращаем `3`
4. Обратно в шаг 2: `rhs = 3`, склеиваем → `Binary(*, 2, 3)`. Loop опять: prec=0<6,
   выходим, возвращаем `(2 * 3)`.
5. Обратно в шаг 1: `rhs = (2 * 3)` → `Binary(+, 1, (2 * 3))`. Loop опять:
   prec=0<1, выходим.

Итог — `1 + (2 * 3)`. Приоритет уважён.

---

### В10. Почему именно `prec(op) + 1`? Что будет, если просто `prec(op)`?

**Ответ.** Это про **ассоциативность**.

`a - b - c` парсится так:
- с `prec + 1`: после потребления первого `-`, рекурсия зовётся с `minPrec=6` (на единицу выше). Внутри `peek = -, prec = 5 < 6` — выход. Возвращаем `b`. Склеиваем `(a - b)`. Потом следующая итерация loop’а потребляет второй `-`. Итог: `(a - b) - c`. **Левая ассоциативность.**
- с `prec` (без +1): рекурсия зовётся с `minPrec=5`. Внутри `peek = -, prec = 5 ≥ 5` — продолжаем. Потребляем `-`, рекурсивно. Итог: `a - (b - c)`. **Правая ассоциативность.**

Для `-` правильна левая (математически `a - b - c = (a - b) - c`). Для `=`
была бы правая (`a = b = c = (a = (b = c))`), но у нас `=` — это statement, не
expression, так что вопрос не возникает.

---

### В11. Что такое error recovery и зачем оно?

**Ответ.** Без recovery компилятор падает на первой ошибке. Студент исправил —
получил следующую — исправил — следующую. Это раздражает. Хочется выдавать
**все ошибки за один проход**.

У меня две функции синхронизации в [Parser.cpp:34-49](../solidity-llvm/src/parser/Parser.cpp#L34-L49):

```cpp
void synchronizeToSemicolon();  // выкинуть всё до ';' включительно
void synchronizeToBraceEnd();   // потребить '{...}' блок целиком
```

Применяются вот так в `parseStateVarDecl` ([Parser.cpp:64-86](../solidity-llvm/src/parser/Parser.cpp#L64-L86)):
```cpp
auto ty = parseType();
if (!ty) { synchronizeToSemicolon(); return nullopt; }
```

Если тип не распарсился — выкидываем токены до `;` и продолжаем парсить
следующую state-переменную. Компилятор не падает, выдаст ошибку и пойдёт
дальше.

Это стандартный приём, описан у Dragon Book («panic mode recovery»).

---

### В12. Если контракт открыт `{`, но не закрыт — что произойдёт?

**Ответ.** В `parseContract` ([Parser.cpp:226](../solidity-llvm/src/parser/Parser.cpp#L226)) последний `expect(RBrace, ...)` сравнивает с
текущим токеном — а это будет `EndOfFile`. `expect` запишет диагностику
`expected RBrace in contract body, got EndOfFile` и вернёт `false`.

Аналогично в `parseBlockStatements` ([Parser.cpp:367-370](../solidity-llvm/src/parser/Parser.cpp#L367-L370)) проверка явно
делается до `expect`:
```cpp
if (check(TokenKind::EndOfFile)) {
    diag_.error(openLoc, "unterminated '{' body");
    return false;
}
```

`openLoc` — это позиция открывающей скобки, чтобы пользователь видел **где
именно** забыл закрыть.

---

### В13. Как ваш парсер отличает объявление локалки (`uint256 x = ...`) от присваивания (`x = ...`)?

**Ответ.** По **первому токену** в [parseStmt](../solidity-llvm/src/parser/Parser.cpp#L375-L444):

```cpp
if (check(KwUint256) || check(KwBool) || check(KwAddress)) {
    // VarDecl
}
if (check(Identifier)) {
    // Assign
}
```

Тип всегда начинается с keyword (`uint256`/`bool`/`address`), а присваивание —
с обычного идентификатора. Это LL(1) — хватает одного токена lookahead.

В реальном Solidity всё сложнее: там есть пользовательские типы, и `MyStruct x = ...`
выглядит как «идентификатор + идентификатор». Это требует либо backtracking,
либо tentative parse, либо больше lookahead. У нас в MVP такого нет.

---

## Раздел 4. Семантика

### В14. Почему Symbol Table — это **стек** скоупов?

**Ответ.** Потому что скоупы вложенные и **симметричные**:
```
global         (контракт)
  fn-scope     (параметры, локалки функции)
    then-scope (локалки внутри if-then)
```

При выходе из `if` — `then-scope` отбрасывается, видны только `fn-scope` и
`global`. Это естественно моделируется стеком: `enterScope` = push,
`leaveScope` = pop. Lookup идёт **сверху вниз** — сначала самый внутренний.

[Symbol.cpp:22-27](../solidity-llvm/src/sema/Symbol.cpp#L22-L27):
```cpp
Symbol* SymbolTable::lookup(string_view name) {
    for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
        if (Symbol* s = it->find(name)) return s;
    }
    return nullptr;
}
```

Это **shadowing**: локалка `x` затеняет state-var `x`. Это поведение Solidity
и большинства языков.

---

### В15. Почему `then`-блок `if` — это отдельный скоуп?

**Ответ.** Чтобы локалки, объявленные в `then`, не пересекались с локалками в
`else` и не утекали наружу:
```solidity
if (cond) { uint256 t = 1; ... }
else      { uint256 t = 2; ... }   // тот же 't' — не конфликт
// здесь 't' невидим
```

В моей Sema это явно в [Sema.cpp:204-209](../solidity-llvm/src/sema/Sema.cpp#L204-L209):
```cpp
symbols_.enterScope();
for (sp : i.thenBlock) checkStmt(*sp);
symbols_.leaveScope();
symbols_.enterScope();
for (sp : i.elseBlock) checkStmt(*sp);
symbols_.leaveScope();
```

Два независимых push/pop — два независимых скоупа.

---

### В16. Объясните, как `value` (state-var) и `delta` (param) различаются в Sema.

**Ответ.** Оба попадают в Symbol Table, но в **разные скоупы** и с **разным
`SymbolKind`**:

1. На `registerContractMembers` ([Sema.cpp:13-35](../solidity-llvm/src/sema/Sema.cpp#L13-L35)) state-vars
   попадают в **глобальный** скоуп с `SymbolKind::StateVar` и `storageSlot=0/1/...`
2. На `analyzeFunction` ([Sema.cpp:241-247](../solidity-llvm/src/sema/Sema.cpp#L241-L247)) параметры
   попадают в **функциональный** скоуп с `SymbolKind::Param` и `storageSlot=-1`.

Когда `checkExpr(IdentExpr "value")` зовёт `lookup`, мы находим Symbol с
`kind=StateVar, slot=0` — и записываем это в `IdentExpr::resolvedKind` и
`resolvedSlot`. Для `delta` находим `kind=Param, slot=-1`.

CodeGen потом смотрит `resolvedKind` и выбирает: для `StateVar` — `sload`,
для `Param` — `load` из alloca.

---

### В17. Почему вы **копируете** поля из `Symbol*` в `IdentExpr`, а не сохраняете указатель?

**Ответ.** Это **самая важная инженерная вещь** в Sema.

`Symbol` живёт в `Scope`, а `Scope` уничтожается при `leaveScope()`. Если бы
`IdentExpr` хранил `Symbol*`, после выхода из функции это был бы dangling
pointer — CodeGen прочёл бы мусор.

Поэтому в [Sema.cpp:52-63](../solidity-llvm/src/sema/Sema.cpp#L52-L63) я копирую нужные **значения** в AST:
```cpp
id.resolvedKind = ...;        // enum, по значению
id.resolvedSlot = sym->storageSlot;  // int, по значению
id.resolvedType = sym->type;  // Type{kind, loc}, по значению
```

AST живёт всю компиляцию, никаких ссылок наружу — codegen безопасно читает.

> «Альтернатива была бы — сделать Symbol-ы owned `unique_ptr` и не удалять при
> `leaveScope`. Но это усложняет управление памятью без выигрыша. Копирование
> трёх полей дешевле.»

---

### В18. Если в `if`-блоке объявить локалку, а потом в `else` — она конфликтнет?

**Ответ.** Нет, не конфликтнет — они в **разных** скоупах (см. В15). Каждый
блок — свой `enterScope`. `declare` падает только если в **текущем верхнем**
скоупе уже есть имя.

Пример:
```solidity
if (cond) { uint256 x = 1; }
else      { uint256 x = 2; }   // OK
```
Оба `x` — независимы.

---

### В19. У вас Sema проверяет, что все пути функции делают return?

**Ответ.** **Нет**. Это явная недоделка MVP. Я могу её поймать на дипломе через
classic «definite return analysis» — обход по CFG.

Сейчас компилятор не упадёт: в [CodeGen.cpp:57-63](../solidity-llvm/src/codegen/CodeGen.cpp#L57-L63) есть safety net:

```cpp
if (!builder_->GetInsertBlock()->getTerminator()) {
    if (func->getReturnType()->isVoidTy()) builder_->CreateRetVoid();
    else builder_->CreateRet(Constant::getNullValue(func->getReturnType()));
}
```

Если в конце нет terminator — добавляем `ret 0`. Это сохраняет IR валидным, но
семантически означает «функция тихо вернёт мусор». В дипломе закрою.

---

## Раздел 5. CodeGen и LLVM

### В20. Что такое SSA и почему вы её не используете напрямую?

**Ответ.** **SSA** = Static Single Assignment. Каждая «переменная» в IR
присваивается **ровно один раз**. Это резко упрощает dataflow-анализ.

Пример non-SSA:
```
x = 1
x = x + 2     ; второе присвоение
```

То же в SSA:
```
x1 = 1
x2 = x1 + 2   ; новое имя
```

При ветвлениях SSA требует **phi-нод**:
```
if (cond) x1 = 1
else      x2 = 2
x3 = phi [x1, then], [x2, else]
```

**Я не пишу SSA вручную**, потому что это сложно: нужно отслеживать активные
версии переменной и вставлять phi-ноды при слиянии. Вместо этого я делаю как
clang:

```cpp
%x = alloca i256   ; выделили "ящик" в стеке
store i256 1, ptr %x   ; присваивание
%v = load i256, ptr %x ; чтение
store i256 5, ptr %x   ; ещё присваивание
```

Это работает: каждое чтение — `load`, каждая запись — `store`, никаких phi.
А LLVM-проход `mem2reg` за один вызов конвертирует это всё в чистый SSA с
phi-нодами автоматически.

Это **стандартный приём clang**.

---

### В21. Что такое `IRBuilder`?

**Ответ.** Это «карандаш» LLVM. Вместо ручного конструирования инструкций (что
требует знания внутреннего API), я пишу:

```cpp
auto* sum = builder_->CreateAdd(L, R, "sum");
```

`IRBuilder` сам:
- вставит инструкцию в **текущий BasicBlock** (заданный через `SetInsertPoint`)
- автоматически проставит SSA-имя (`%sum`)
- проверит типы (если `L` — `i256`, а `R` — `i1`, бросит assertion)

`SetInsertPoint(bb)` — это «пиши теперь сюда». Меняя его, я переключаюсь
между блоками (например, при генерации `if/else`).

---

### В22. Почему `i256`? Никакого x86 такого нет.

**Ответ.** Правильно — нативно ни один процессор не умеет 256-битные
integer’ы. Но LLVM поддерживает **integer любой ширины** — `i256`, `i512`,
`i1024`. Это и есть **ключевая причина**, почему мы выбрали LLVM.

Для **анализа** этого хватает. Анализаторам (dataflow, dominator tree,
reaching definitions) не важно, какой ширины операнды — они работают со
структурой IR, не с реальным execution.

Если бы я попытался запустить `llc` на моём IR, он бы упал: x86-кодогенерация
не умеет `i256`. **Но я и не собираюсь это запускать** — компилятор для
анализа, не для исполнения. На защите можно сказать:

> «Цель — статический анализ. `llc` бы упал, и это ожидаемо. Когда речь
> пойдёт о реальном bytecode, я буду переводить из LLVM IR в EVM — но это
> уже за рамками работы.»

---

### В23. Почему деление — `udiv`, а не `sdiv`? Сравнения — `ult`/`ugt`?

**Ответ.** Потому что `uint256` — **беззнаковый** тип. В LLVM есть две версии
многих инструкций:
- `udiv` (unsigned), `sdiv` (signed)
- `ult` (unsigned less than), `slt` (signed)
- `urem`/`srem`, и т.д.

Беззнаковая интерпретация рассматривает 256 битов как `[0, 2^256-1]`,
знаковая — как `[-2^255, 2^255-1]`. Для `uint256` — только unsigned.

Если бы я ошибся и использовал `sdiv`, число `2^255` интерпретировалось бы
как отрицательное, и результат был бы не тот.

---

### В24. У вас `!` сделан как `xor v, 1`. Корректно?

**Ответ.** Корректно **только для `i1`** (bool). Для `i1`:
- `not(0) = 0 ^ 1 = 1`
- `not(1) = 1 ^ 1 = 0`

Это и есть логическое отрицание.

Для `i256` `xor v, 1` дал бы битовое отрицание младшего бита, а это не
логическое NOT. **Но в нашем коде это безопасно**, потому что Sema гарантирует:
операнд `!` обязан быть `bool`. Sema падает с ошибкой `operator '!' expects
bool` ([Sema.cpp:113-115](../solidity-llvm/src/sema/Sema.cpp#L113-L115)), если не bool.

Это пример **delegation of invariant** — codegen верит семантике.

---

### В25. У вас `&&` и `||` без short-circuit. Это правильно?

**Ответ.** Корректно **только потому, что в нашем подмножестве у выражений нет
побочных эффектов**. В Solidity `&&` и `||` short-circuit, как и в C. Но
короткое замыкание видимо только когда правый операнд имеет side-effect:
вызов функции, который что-то меняет.

В моём MVP функциональных вызовов как expression нет (CallExpr запрещён в
Sema [Sema.cpp:144-149](../solidity-llvm/src/sema/Sema.cpp#L144-L149)). Поэтому
`a && b` и `a & b` дают идентичный результат — оба операнда чистые. Я эмичу
bitwise:
```cpp
case BinOp::AndAnd: return builder_->CreateAnd(L, R, "and");
case BinOp::OrOr:   return builder_->CreateOr(L, R, "or");
```

На дипломе, когда добавлю вызовы, переделаю на ветвление через BasicBlock’и —
классический паттерн lazy-evaluation:
```
%cond = ...
br i1 %cond, label %eval_rhs, label %skip
eval_rhs:
  %rhs = call ...
  br label %skip
skip:
  %result = phi [false, %entry], [%rhs, %eval_rhs]
```

---

### В26. Что такое intrinsic в вашем контексте?

**Ответ.** В обычном LLVM intrinsic — это псевдо-функция типа `@llvm.memcpy`,
которую оптимизатор знает «изнутри». У меня **свои собственные** intrinsics:

```llvm
declare i256 @evm.sload(i256)
declare void @evm.sstore(i256, i256)
declare void @evm.revert()                ; noreturn
declare i160 @evm.msg_sender()
declare i256 @evm.msg_value()
```

Это **функции без тела** (declarations, не definitions). LLVM-формат это
разрешает: вызов `call void @evm.sstore(...)` валиден без реализации, если
функция объявлена.

**Зачем они нужны.** EVM имеет специфические операции, которых нет в обычных
языках: чтение/запись в storage, msg.sender, revert. Их нельзя выразить через
обычные LLVM-инструкции. Я представляю их через intrinsics, которые на
дипломе будут **маркерами** для анализаторов:

- `@evm.sload`/`@evm.sstore` — обращения к storage. Поиск reentrancy: «есть
  ли внешний call между sload и sstore?»
- `@evm.msg_sender` — проверки прав. Поиск tx.origin misuse.
- `@evm.revert` — точки отказа. Может пригодиться для path-coverage анализа.

Это и есть **научное содержание моей работы**: я не делаю произвольный
codegen, а вношу EVM-семантику в LLVM IR через стандартный паттерн intrinsics.

---

### В27. Почему `evm.revert` с `noreturn`? И зачем `unreachable` после?

**Ответ.** `noreturn` — это атрибут функции, который говорит оптимизатору
«управление **никогда не возвращается** из этого вызова». Я ставлю это через
`setDoesNotReturn()` в [EvmIntrinsics.cpp:24](../solidity-llvm/src/codegen/EvmIntrinsics.cpp#L24).

**Зачем `unreachable` после?** В LLVM каждый BasicBlock обязан заканчиваться
**terminator’ом**: `ret`, `br`, `switch`, `unreachable`, `invoke`. Обычный
`call` — **не terminator**. Поэтому после вызова `@evm.revert` я должен явно
закрыть блок:

```llvm
require.fail:
  call void @evm.revert()
  unreachable    ; ← terminator, означает "сюда control никогда не дойдёт"
```

`unreachable` — это и terminator (LLVM-verify довольный), и подсказка
оптимизатору, что путь мёртвый: можно выкинуть инструкции после, не вставлять
phi-ноду из этого блока и т.д.

Если убрать `unreachable`, `verifyModule` выдаст ошибку «BasicBlock without
terminator».

---

### В28. Что произойдёт с `if.merge`-блоком, если обе ветки делают return?

**Ответ.** В [CodeGen.cpp:217-221](../solidity-llvm/src/codegen/CodeGen.cpp#L217-L221):

```cpp
if (mergeBB->hasNPredecessors(0)) {
    mergeBB->eraseFromParent();
} else {
    builder_->SetInsertPoint(mergeBB);
}
```

Если обе ветки `then` и `else` закончились `return` — они **сами стали
terminated**, и в `mergeBB` никто не идёт. Это «мёртвый блок» без
предшественников.

LLVM-verify не любит блоки без предшественников и без `unreachable`. Поэтому
я его явно удаляю: `eraseFromParent()`.

Если хотя бы одна ветка дошла до merge (была без return) — там есть
предшественник, оставляем блок и ставим в нём insert point для следующих
statement’ов.

---

### В29. Покажите, как `value = value + delta` превратится в IR.

**Ответ.** Дано: `value` — state-var в slot 0, `delta` — параметр функции.

1. **AssignStmt** видит target = `"value"`, value = `BinaryExpr(Add, Ident "value", Ident "delta")`.
2. Идём в `emitExpr(BinaryExpr)`:
   - `emitExpr(Ident "value")`: `resolvedKind = StateVar, slot = 0`. Вызываем
     `emitStateLoad(0, Uint256)`:
     ```llvm
     %sload = call i256 @evm.sload(i256 0)
     ```
     Возвращает `%sload`.
   - `emitExpr(Ident "delta")`: `resolvedKind = Param`. Ищем в `locals_`:
     ```llvm
     %delta1 = load i256, ptr %delta, align 4
     ```
   - `CreateAdd(L, R, "add")`:
     ```llvm
     %add = add i256 %sload, %delta1
     ```
3. Обратно в `emitStmt(Assign)`. Target `value` — state-var (нет в `locals_`).
   `emitStateStore(0, %add, Uint256)`:
   ```llvm
   call void @evm.sstore(i256 0, i256 %add)
   ```

Итог:
```llvm
%sload = call i256 @evm.sload(i256 0)
%delta1 = load i256, ptr %delta, align 4
%add = add i256 %sload, %delta1
call void @evm.sstore(i256 0, i256 %add)
```

---

### В30. Покажите, как `require(msg.sender == owner)` превращается в IR.

**Ответ.** Дано: `owner` — state-var типа `address` в slot 1.

1. **emitStmt(Require)** идёт в [CodeGen.cpp](../solidity-llvm/src/codegen/CodeGen.cpp).
2. `emitExpr(BinaryExpr Eq, MemberExpr msg.sender, Ident "owner")`:
   - `emitExpr(MemberExpr msg.sender)` — синтаксически распознаём `msg.sender`,
     эмитим:
     ```llvm
     %msg.sender = call i160 @evm.msg_sender()
     ```
   - `emitExpr(Ident "owner")` — это StateVar, slot=1, тип `address` (i160).
     `emitStateLoad(1, Address)`:
     ```llvm
     %sload = call i256 @evm.sload(i256 1)
     %trunc = trunc i256 %sload to i160      ; storage = 256 бит, address = 160
     ```
   - `CreateICmpEQ(L, R, "eq")`:
     ```llvm
     %eq = icmp eq i160 %msg.sender, %trunc
     ```
3. Создаём два блока, conditional branch, fail-блок с revert+unreachable,
   ставим insert в ok:
   ```llvm
   br i1 %eq, label %require.ok, label %require.fail

   require.ok:
     ; ... продолжение функции ...

   require.fail:
     call void @evm.revert()
     unreachable
   ```

Это **ровно тот IR**, который выдаёт компилятор на `tests/fixtures/counter.sol`
— можно показать вживую.

---

### В31. Зачем `trunc i256 to i160` при чтении `owner`?

**Ответ.** EVM-storage хранит ячейку **всегда 256 бит**. Когда я читаю
`owner` (type `address` = 160 бит), `sload` возвращает `i256`, и мне нужно
**обрезать** до `i160`:

```cpp
auto* raw = builder_->CreateCall(intr_.sload, {slotConst}, "sload");
if (targetTy->isIntegerTy(256)) return raw;
return builder_->CreateTrunc(raw, targetTy, "trunc");
```

Это копирует семантику Solidity: «storage — это i256, а тип переменной — view
сверху». Адрес — это младшие 160 бит ячейки, остальные 96 — должны быть нулями.

Обратное (при записи) — `zext`:
```cpp
if (!v->getType()->isIntegerTy(256)) {
    wide = builder_->CreateZExt(v, i256, "zext");
}
```

`zext` = zero-extend = «забить нулями старшие биты». Это безопасное расширение
для беззнаковых типов.

---

### В32. Что валидирует `verifyModule`, что валидирует `llvm-as`?

**Ответ.** Это два разных уровня проверки.

**`verifyModule`** — встроенный в LLVM валидатор C++ API:
- каждый BasicBlock имеет terminator
- типы операндов совпадают
- SSA-форма (если включена) соблюдена
- интринзики используются с правильными сигнатурами
- доминирование (каждое использование SSA-имени доминируется его
  определением)

Я зову его в [CodeGen.cpp:259](../solidity-llvm/src/codegen/CodeGen.cpp#L259) — если он сказал «ок», IR
**гарантированно валидный**.

**`llvm-as`** — внешний инструмент, **парсит .ll-текст в .bc-бинарник**. Это
проверка:
- IR корректно сериализован в текст
- текст обратно парсится без ошибок
- все объявления непротиворечивы

Это два уровня: `verifyModule` — на уровне C++ объектов, `llvm-as` — на уровне
текстовой формы. Если оба зелёные — я уверен, что IR полностью валиден.

Я гоняю оба:
- `verifyModule` — внутри codegen, на каждом компиляции
- `llvm-as` — внешне в командной строке для финальной проверки

---

## Раздел 6. Driver и тесты

### В33. Почему в Driver всё через `raw_string_ostream`, а не сразу в файл?

**Ответ.** LLVM использует **собственные** потоки (`llvm::raw_ostream`), не
`std::ostream`. Нельзя написать `std::cout << mod`, потому что `mod->print()`
ожидает `raw_ostream&`.

Я делаю мостик в [Driver.cpp:61-64](../solidity-llvm/src/driver/Driver.cpp#L61-L64):
```cpp
std::string ir;
llvm::raw_string_ostream os(ir);
mod->print(os, nullptr);
os.flush();
```

Сначала печатаю в строку, потом строку — в `std::ofstream` или `std::cout`.
Это стандартный паттерн «LLVM → std».

Альтернативно можно было бы использовать `llvm::raw_fd_ostream` напрямую — но
тогда обработка `outputPath = ""` (т.е. stdout) усложнилась бы. Через строку
проще.

---

### В34. Сколько у вас тестов? Какой фреймворк?

**Ответ.** **97 unit-тестов**, фреймворк — GoogleTest. Они подтянуты через
FetchContent в CMake (см. [cmake/FetchDeps.cmake](../solidity-llvm/cmake/FetchDeps.cmake)). Запускаются `ctest --output-on-failure`.

Разбивка:
- `test_smoke.cpp` — 2 базовых
- `test_lexer.cpp` — ~15 тестов на токенизацию
- `test_parser.cpp` — ~30 тестов на грамматику + recovery
- `test_sema.cpp` — ~30 тестов на проверки типов, scope, storage slot
- `test_codegen.cpp` — ~20 тестов на эмиссию IR + интеграционный Counter

Главный **интеграционный тест** — `CodeGen.CounterFullVerifies`: берёт настоящий
`counter.sol` из CLAUDE.md, гонит через весь pipeline, проверяет что IR
валиден через `verifyModule`.

---

### В35. Чем проверяется, что IR действительно корректный?

**Ответ.** Двумя уровнями:
1. **`verifyModule`** внутри codegen — проверка SSA, terminators, типов
2. **`llvm-as`** — внешняя сериализация туда-обратно

Помимо этого, на каждый отдельный аспект codegen есть юнит-тест:
- `CodeGen.AddProducesIRAdd` — что `+` действительно стал `add` инструкцией
- `CodeGen.IfElseEmitsBranchAndBlocks` — что появились блоки и conditional branch
- `CodeGen.RequireEmitsRevertBranch` — ровно 1 `call @evm.revert` + 1 `unreachable`

Если бы где-то была опечатка (например, `add` вместо `mul`), такой тест
поймал бы. `verifyModule` не поймал бы, потому что `add` и `mul` оба
синтаксически валидны.

---

## Раздел 7. Caверзные

### В36. Что произойдёт, если входной файл пустой?

**Ответ.** Лексер сразу выдаст `EndOfFile`. Парсер в `parseContract` зайдёт
на `if (!check(KwContract))` — ошибка «expected 'contract' at top level», вернёт
`nullopt`. Driver увидит, что contract = `nullopt`, напечатает диагностики и
вернёт exit code 1. Никакого падения, IR не сгенерируется.

---

### В37. Что если попросить компилировать несуществующий файл?

**Ответ.** [Driver.cpp:27-31](../solidity-llvm/src/driver/Driver.cpp#L27-L31):
```cpp
std::ifstream in(opts.inputPath);
if (!in) {
    std::cerr << "error: cannot open '" << opts.inputPath << "'\n";
    return 1;
}
```
Один из тестов в `test_smoke.cpp` это явно проверяет.

---

### В38. Если две функции с одинаковым именем — что произойдёт?

**Ответ.** В `registerContractMembers` второй `symbols_.declare(Symbol{Function, ...})`
вернёт `false` (потому что `Scope::add` использует `emplace`, который не
перезаписывает). Sema запишет ошибку:
```
error: duplicate function 'foo'
```
и продолжит обрабатывать остальные функции — recovery работает.

---

### В39. Что произойдёт на `return;` в функции, объявленной как `returns (uint256)`?

**Ответ.** Парсер в `parseStmt` ожидает выражение после `return`:
```cpp
auto val = parseExpr();
if (!val) return nullptr;
```
Если после `return` сразу `;`, `parseExpr()` упадёт с ошибкой «unexpected token
in expression». Так что синтаксически такой return даже не дойдёт до Sema.

В коде Sema есть ветка для `return;` без значения (на случай если парсер позволит),
она проверит, что `currentReturnType_ == nullptr`. Эта ветка пока недостижима,
но оставлена как safety net.

---

### В40. Если в `counter.sol` поменять имя state-var с `value` на `value_2`, всё заработает?

**Ответ.** Да. Имя не имеет значения, главное — порядок объявления (это
определяет slot). `value_2` всё равно станет slot 0, потому что объявлен первым.
Лексер пропускает `_` в идентификаторах, парсер делает `string(lexeme)`.

---

### В41. Что вы добавите первым делом на дипломе?

**Ответ.** Три приоритета:
1. **`mapping`** — главная структура storage в Ethereum, без неё ни один
   реальный контракт не разобрать.
2. **Циклы `for`/`while`** — без них даже простые алгоритмы (сумма массива) не
   выражаются.
3. **Modifier** — это синтаксический сахар над `require`, но индустрия их
   использует везде (`onlyOwner`, `nonReentrant` и т.п.).

После этого можно браться за первый pass-детектор — **reentrancy detector**.

---
