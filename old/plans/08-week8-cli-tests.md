# Week 8 — CLI, Tests, Documentation

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans.

**Goal:** Довести проект до защищаемой формы: production-CLI (cxxopts с валидацией и понятными ошибками), полный набор unit + integration тестов (5–7+), README, пояснительная записка к курсовой. После этой недели — коммит-хэш можно показывать руководителю.

**Architecture:** Этот план — полировка. Новой логики нет, только CLI-UX, тесты, документация.

**Tech Stack:** cxxopts, GoogleTest, markdown.

---

## Files

- Modify: `src/main.cpp` — улучшенные опции, `--version`, `--ast-dump`, валидация
- Modify: `src/driver/Driver.cpp` — флаги `dumpAst`, `dumpTokens`
- Create: `tests/test_integration.cpp` — e2e через CLI
- Create: `README.md` (переписать финально)
- Create: `docs/coursework_report.md` — черновик пояснительной записки (в репе или вне, по выбору пользователя)

---

## Tasks

### Task 1: CLI — версия, валидация, сообщения об ошибках
- [ ] Опции: `--input <path>` (required), `--output <path>`, `--emit-ir`, `--ast-dump`, `--tokens`, `--version`, `--help`.
- [ ] Валидация: если нет `--input` — понятное сообщение и exit 1.
- [ ] `--version` печатает `solidity-llvm 0.1.0 (LLVM 18.1.x)`.
- [ ] Тест: `CLI.NoInputFailsWithMessage` — запуск бинаря без аргументов возвращает != 0 и stderr содержит "input".
- [ ] Commit: `feat(cli): complete option handling and messages`

### Task 2: `--ast-dump` и `--tokens`
- [ ] При `--tokens`: после лексера печатает поток токенов (kind + lexeme + loc).
- [ ] При `--ast-dump`: после парсинга печатает AST через имеющийся `dump`.
- [ ] Эти режимы не требуют sema/codegen — полезны для отладки.
- [ ] Тесты: запуск с `--tokens` на counter.sol — stdout содержит "Identifier" и "KwContract".
- [ ] Commit: `feat(cli): --tokens and --ast-dump debug flags`

### Task 3: End-to-end integration tests через `add_test`
- [ ] В `tests/CMakeLists.txt` добавить `add_test` для каждой фикстуры:
  - `Integration.Counter` — собирает счёт и llvm-as.
  - `Integration.SimpleAdd`.
  - `Integration.OnlyStorage`.
  - `Integration.WithRequire`.
- [ ] Каждый тест — запуск бинаря + `llvm-as`, exit 0.
- [ ] Commit: `test: integration tests via ctest fixtures`

### Task 4: Негативные интеграционные тесты
- [ ] `tests/fixtures/errors/bad_type.sol`: `int256 x;` — парсер/sema должен ругнуться, exit != 0.
- [ ] `tests/fixtures/errors/undecl.sol`: `return unknownVar;`.
- [ ] `tests/fixtures/errors/mismatch.sol`: `return true;` в функции с `returns (uint256)`.
- [ ] По тесту на каждый — ожидается exit != 0 и stderr с понятным сообщением.
- [ ] Commit: `test: negative integration tests`

### Task 5: Чистый README.md
- [ ] Структура:
  - Что это
  - Сборка (WSL + Ubuntu 24.04 + LLVM 18)
  - Примеры запуска (--help, --emit-ir, --tokens, --ast-dump)
  - Поддерживаемое подмножество (с таблицей типов)
  - Пример: Counter.sol → IR (с примером вывода)
  - Ограничения / что не поддержано
  - Дорожная карта до диплома: passes, список детекторов
- [ ] Commit: `docs: comprehensive README`

### Task 6: Пояснительная записка (черновик)
- [ ] `docs/coursework_report.md` или `.pdf`-скелет:
  1. Введение и мотивация (уязвимости смарт-контрактов, LLVM как платформа)
  2. Обзор существующих инструментов (Slither, Mythril, Solang) — коротко
  3. Архитектура компилятора (диаграмма: .sol → tokens → AST → typed AST → LLVM IR)
  4. Подмножество языка (с примерами и обоснованием отсечений)
  5. EVM-модель через intrinsics (таблица)
  6. Примеры трансляций (simple add, if, require, msg.sender, state vars)
  7. Тестирование и критерии корректности
  8. Ограничения и план развития до диплома
  9. Заключение
  10. Список литературы (LLVM Language Reference, Solidity docs, статьи по Slither/Mythril)
- [ ] Commit: `docs: coursework report draft`

### Task 7: Финальная верификация
- [ ] Чистая пересборка из нуля.
- [ ] `ctest --output-on-failure` — все тесты зелёные.
- [ ] Выполнить вручную:
  ```bash
  ./solidity-llvm --input tests/fixtures/full_counter.sol --emit-ir --output /tmp/counter.ll
  llvm-as /tmp/counter.ll
  opt -O2 /tmp/counter.ll -S -o /tmp/counter.opt.ll   # бонус, не обязательно
  ```
- [ ] Проверить подсчёт: ≥ 30 unit-тестов суммарно (lexer + parser + sema + codegen + cli).
- [ ] Commit: `chore: week 8 complete — coursework-ready`

### Task 8: Тег
- [ ] `git tag -a v0.1.0-coursework -m "Coursework MVP: Solidity subset → LLVM IR"`.
- [ ] Commit не нужен, просто тег.

---

## Критерий готовности недели 8 (и курсовой)

- [ ] `./solidity-llvm --help` и `--version` работают
- [ ] `--ast-dump`, `--tokens` дают полезный вывод
- [ ] `ctest` — все тесты зелёные (unit + integration + negative)
- [ ] Counter.sol end-to-end через CLI, `llvm-as` принимает
- [ ] README содержит всё нужное для сборки и запуска на чистой машине
- [ ] Черновик пояснительной записки готов
- [ ] Тег `v0.1.0-coursework` поставлен

---

## Что дальше (после курсовой — диплом)

Не входит в курсовую, но на заметку:

1. Расширение языка: `int*`, `mapping`, `struct`, `for`, `while`, `constructor`, `modifier`, `event`, внешние вызовы.
2. Переход от stub-intrinsics к **CustomDialect** или явному pass-pipeline: подключить реальный анализ.
3. Реализация LLVM passes:
   - `ReentrancyDetector` — сканирует CallExpr через DominatorTree, ищет вызов `call_external` перед `evm.sstore`.
   - `TxOriginDetector` — поиск сравнений с `tx.origin`.
   - `UnprotectedEtherDetector` — функции с `callvalue > 0` без require на caller.
   - `StorageCollisionDetector` — нахождение записей в один слот через разные пути.
4. Бенчмарки и сравнение с Slither/Mythril.
5. Оформление диплома.
