# IR-контракт SCSA

> Формальная спецификация LLVM IR, которую **обязан** эмитить любой фронтенд
> (рукописный codegen ИЛИ будущий `AstJsonLowering` из solc-AST), чтобы downstream
> — детекторы, KLEE-понижение, генератор PoC — работал **без изменений**.
>
> **Инвариант (проверен):** весь downstream (`src/analyzer`, `src/klee`, `src/report`)
> содержит `ast::` **0 раз**. Он читает только конструкции ниже. Значит фронтенд
> полностью заменяем — это фундамент переезда на Solidity 0.8 (гибрид B→D).
>
> Ground-truth: `solidity-llvm --emit-ir` на `benchmarks/corpus/reentrancy_mapping.sol`
> и `reentrancy_classic.sol`. Фаза 2 должна воспроизвести это байт-в-байт по структуре.

## 1. Объявления интринсиков (эмитить всегда, один раз на модуль)

```llvm
declare void            @evm.revert()                       ; noreturn (require/revert/throw)
declare i160            @evm.msg_sender()
declare i256            @evm.msg_value()
declare i160            @evm.tx_origin()
declare i256            @evm.block_timestamp()
declare i256            @evm.block_number()
declare void            @evm.selfdestruct(i160)
declare i1              @scsa.external_call(i160, i256)      ; CALL — управление уходит наружу
declare ptr addrspace(1) @scsa.map_elem(i256, i256)         ; (baseSlot, key) -> элемент mapping
declare void            @scsa.opaque_store()                ; маркер записи в struct-поле/массив после вызова
```

## 2. Storage (постоянное состояние) — addrspace(1) + метаданное слота

Слот-идентичность несут ДВА независимых носителя (downstream читает оба):
1. `!scsa.storage !{i64 <slot>}` на инструкции load/store — читает `ReentrancyDetector`,
   `AccessControlDetector`, `IRTrace`.
2. Сам указатель как **константа** — читает `KleeLowering::constSlotOfPtr` (KLEE требует
   константный слот; рантайм-слот = провал).

**Скалярная state-var на слоте N:**
```llvm
; указатель = inttoptr(N to ptr addrspace(1)); для N==0 канонизируется в null
%v = load  i256, ptr addrspace(1) null, align 4, !scsa.storage !{i64 0}
     store i256 0, ptr addrspace(1) null, align 4, !scsa.storage !{i64 0}
```

**Элемент mapping (slot базы = N, ключ символьный/вычислимый):**
```llvm
%k  = zext i160 %msg.sender to i256
%p  = call ptr addrspace(1) @scsa.map_elem(i256 N, i256 %k)   ; N — КОНСТАНТА (базовый слот)
%v  = load  i256, ptr addrspace(1) %p, align 4, !scsa.storage !{i64 N}
      store i256 %new, ptr addrspace(1) %p, align 4, !scsa.storage !{i64 N}
```

Правила слотов (как в `sema::Sema` + `Inheritance`): нумерация с 0 в порядке объявления,
базовые контракты первыми (C3). **В Solidity 0.8 берём готовые слоты из solc
`storageLayout` — не пересчитываем сами** (Фаза 3/5).

## 3. Внешний вызов `addr.call{value: v}("")`

```llvm
%ok = call i1 @scsa.external_call(i160 %target, i256 %value),
        !scsa.call.kind !{!"call"},        ; call | delegatecall | staticcall
        !scsa.call.trusted !{i1 false}     ; доверенная граница адреса
```
Точка, после которой запись в storage = reentrancy (нарушение CEI). `ReentrancyDetector`
ищет «store в addrspace(1), достижимый ПОСЛЕ этого вызова».

## 4. Контекст транзакции — интринсики (KLEE символизирует)

`msg.sender`→`@evm.msg_sender`, `msg.value`→`@evm.msg_value`, `tx.origin`→`@evm.tx_origin`,
`block.timestamp/number`→`@evm.block_*`. В `AccessControlDetector` owner-слот выводится из
`icmp eq (@evm.msg_sender, load addrspace(1))`.

## 5. Revert / require / throw

Условие инвертируется, на fail-ветке:
```llvm
require.fail:
  call void @evm.revert()
  unreachable
```

## 6. Метаданные на функции

- `!scsa.unmodeled_modifier` на `define` — фронтенд НЕ смог заинлайнить модификатор
  (защита может быть в нём). Детекторы access-control/pattern **пропускают** такую функцию,
  чтобы не выдать ложное срабатывание.
  **В solc-мосте (Фаза 2) тело модификатора разрешено → инлайним → этот маркер и его
  ложные срабатывания исчезают.**

## 7. Прочее

- `selfdestruct(addr)` → `call void @evm.selfdestruct(i160 %addr)`.
- Запись в struct-поле/элемент массива ПОСЛЕ внешнего вызова, когда точный слот неизвестен
  → `call void @scsa.opaque_store()` (честная граница: LOW-предупреждение вместо тихого FN).
  **В Фазе 5 заменяется реальным слотом из solc `storageLayout.types[...].members[].slot`.**
- Debug-info: каждая инструкция несёт `!dbg` из `!DILocation(line, column)` → находка знает строку.
  Источник строки в solc-мосте — поле `src` ("start:len:file") узла AST.

## 8. Что downstream НЕ читает (свобода фронтенда)

Имена SSA-значений, порядок инструкций внутри блока (кроме «после external_call»),
конкретные типы шире i256, layout memory(as2)/calldata(as3). Всё это фронтенд волен менять.
