# Solidity → LLVM IR Compiler — Overview Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement each weekly plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Построить MVP-компилятор подмножества Solidity → LLVM IR за 8 недель. Демо-контракт `Counter.sol` должен end-to-end транслироваться в `.ll`, валидный для `llvm-as`.

**Architecture:** Классический трёхступенчатый фронтенд: lexer → parser (recursive-descent) → semantic analysis (symbol table + type check) → codegen (LLVM IRBuilder). Всё 256-битное представлено как `i256`; EVM-специфика (`msg.sender`, `sstore`, `sload`, `require`) — через декларируемые внешние intrinsic-функции, без реальной реализации. Одиночный бинарь CLI `solidity-llvm`.

**Tech Stack:** C++20, CMake 3.20+, Ninja, LLVM 18.1, GoogleTest (unit), cxxopts (CLI), FetchContent (без vcpkg).

---

## Недельные планы

Каждая неделя — отдельный план с собственными задачами, тестами и коммитами. Выполнять последовательно.

| # | Файл | Артефакт на выходе |
|---|---|---|
| 1 | [01-week1-skeleton.md](01-week1-skeleton.md) | `solidity-llvm/` с CMake, линкующийся с LLVM 18, стабовый CLI, GoogleTest-тест проходит |
| 2 | [02-week2-lexer.md](02-week2-lexer.md) | Лексер со всеми токенами подмножества, зелёный `test_lexer` |
| 3 | [03-week3-parser-decls.md](03-week3-parser-decls.md) | Парсер объявлений: `contract`, state vars, function-header |
| 4 | [04-week4-parser-stmts.md](04-week4-parser-stmts.md) | Парсер тел функций: statements + expressions, полный AST `Counter.sol` |
| 5 | [05-week5-sema.md](05-week5-sema.md) | Semantic analyzer: symbol table, резолвинг имён, проверка типов |
| 6 | [06-week6-codegen-core.md](06-week6-codegen-core.md) | Codegen: storage layout, state vars, arith, `if`, `return` → первый валидный `.ll` |
| 7 | [07-week7-codegen-evm.md](07-week7-codegen-evm.md) | `require`, `msg.sender`, `msg.value`, EVM intrinsics; `.ll` проходит `llvm-as` на всех тестах |
| 8 | [08-week8-cli-tests.md](08-week8-cli-tests.md) | Production CLI, 5-7 unit-тестов, README, пояснительная записка |

---

## Файловая структура проекта

```
solidity-llvm/
├── CMakeLists.txt
├── README.md
├── cmake/
│   └── FetchDeps.cmake              # cxxopts, GoogleTest через FetchContent
├── include/solc/
│   ├── lexer/
│   │   ├── Token.h                  # enum TokenKind + struct Token
│   │   └── Lexer.h                  # class Lexer
│   ├── ast/
│   │   ├── AST.h                    # все узлы: Contract, Function, Stmt, Expr
│   │   └── Visitor.h                # базовый visitor (для codegen)
│   ├── parser/
│   │   └── Parser.h                 # class Parser
│   ├── sema/
│   │   ├── Symbol.h                 # Scope, Symbol
│   │   ├── Type.h                   # enum Ty { U256, Bool, Address }
│   │   └── Sema.h                   # class SemanticAnalyzer
│   ├── codegen/
│   │   ├── StorageLayout.h          # слоты state vars
│   │   ├── EvmIntrinsics.h          # декларации @evm.sload/sstore/caller/...
│   │   └── CodeGen.h                # class CodeGen
│   ├── diag/
│   │   └── Diagnostic.h             # SourceLoc + DiagnosticEngine
│   └── driver/
│       └── Driver.h                 # high-level пайплайн: compile(path) → .ll
├── src/
│   ├── lexer/Lexer.cpp
│   ├── parser/Parser.cpp
│   ├── ast/AST.cpp                  # dump() для отладки
│   ├── sema/Sema.cpp
│   ├── codegen/
│   │   ├── CodeGen.cpp
│   │   ├── StorageLayout.cpp
│   │   └── EvmIntrinsics.cpp
│   ├── diag/Diagnostic.cpp
│   ├── driver/Driver.cpp
│   └── main.cpp                     # CLI (cxxopts)
├── tests/
│   ├── CMakeLists.txt
│   ├── test_lexer.cpp
│   ├── test_parser.cpp
│   ├── test_sema.cpp
│   ├── test_codegen.cpp
│   ├── test_integration.cpp        # .sol → .ll → llvm-as
│   └── fixtures/
│       ├── counter.sol             # демо-контракт
│       ├── simple_add.sol
│       ├── only_storage.sol
│       └── errors/                 # для негативных тестов sema
└── scripts/
    └── run_llvm_as.sh              # wrapper для интеграционных тестов
```

**Принципы декомпозиции:**
- Один header / один .cpp на компонент.
- AST-узлы — POD-структуры (без наследования кроме дискриминатора `Kind`), владение через `std::unique_ptr`.
- Lexer/Parser/Sema/CodeGen — независимые stateful классы, каждый с собственным диагностическим контекстом.
- `Driver` — единственная точка оркестрации пайплайна; `main.cpp` делает только разбор CLI и вызов Driver.

---

## Типы и EVM-модель (общая для всех недель)

**Solidity тип → LLVM тип:**

| Solidity | LLVM |
|---|---|
| `uint256` | `i256` |
| `bool` | `i1` |
| `address` | `i160` |

**Storage layout:** каждая state variable получает слот `i256` (nextSlot++). Адрес и bool при `sstore`/`sload` расширяются до `i256` (`zext`), при чтении — обрезаются (`trunc`).

**EVM intrinsics (декларируются в модуле, без тела):**

```llvm
declare i256 @evm.sload(i256 slot)
declare void  @evm.sstore(i256 slot, i256 value)
declare i160 @evm.caller()
declare i256 @evm.callvalue()
declare void @evm.revert(i256 offset, i256 length)
```

`require(cond)` → `br i1 %cond, label %ok, label %revert`; в блоке `%revert` — `call void @evm.revert(i256 0, i256 0)` + `unreachable`.

---

## Коммит-дисциплина

- Каждая задача из недельного плана — отдельный коммит (conventional commits: `feat:`, `test:`, `refactor:`, `build:`, `docs:`).
- После каждого зелёного теста — коммит. После `git add` — явный список файлов, не `git add -A`.
- Ветка `main`. Для крупных шагов возможны feature-бранчи, но для курсовой — не обязательно.

---

## Критерии готовности (курсовая)

1. `./solidity-llvm --input tests/fixtures/counter.sol --emit-ir --output counter.ll` — успешно.
2. `llvm-as counter.ll` — exit 0.
3. `ctest` — все тесты зелёные (≥5 unit + 1 integration).
4. Пояснительная записка содержит: архитектуру, описание подмножества, примеры трансляции, ограничения, план до диплома.

---

## Self-review checklist (после написания всех 8 планов)

- [ ] Каждая запись scope_coursework.md имеет соответствующую задачу в одном из планов.
- [ ] Нет placeholder-шагов («add error handling», «write tests»).
- [ ] Типы и сигнатуры согласованы между планами (`Lexer::lex`, `Parser::parseContract`, `CodeGen::generate` — единообразно).
- [ ] Каждый план заканчивается валидным артефактом (зелёный тест или рабочий бинарь).
