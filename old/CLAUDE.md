# Дипломный проект — контекст Claude Code (WSL side)

> Этот файл лежит на WSL-стороне (`~/diplom/CLAUDE.md`) и автоматически подгружается, когда Claude Code запускается в папке `~/diplom/`. Работа ведётся здесь: сборка, компиляция, тесты.

---

## Что за проект

**Тема диплома:**
«Разработка компилятора подмножества языка Solidity в промежуточное представление LLVM для статического анализа безопасности смарт-контрактов».

**Суть:** написать C++ фронтенд, который принимает `.sol` файл и выдаёт валидный LLVM IR (`.ll`). Все EVM-специфичные операции (storage, external calls, msg.sender) выражены через intrinsic-функции LLVM. Все 256-битные значения → `i256`. В дальнейшем (на этапе диплома) поверх IR пишутся LLVM passes для детекции уязвимостей (reentrancy, tx.origin, unprotected ether, storage collision).

**Сейчас идёт фаза курсовой работы (2 месяца)** — реализация только фронтенда MVP. Научная часть (passes) — после курсовой.

---

## Где что лежит

### Здесь (WSL Ubuntu 24.04)
- `~/diplom/solidity-llvm/` — **основной проект компилятора** (C++20, CMake, LLVM 18). Создаётся на неделе 1.
- `~/diplom/hello-llvm/` — учебный bootstrap (проверка что IRBuilder работает, создан по setup_wsl.md этап 4).

### Windows (видны из WSL как `/mnt/c/Users/akum1/diplom/`)
Ключевые документы проекта:
- [`доки/tz_llvm_frontend.md`](/mnt/c/Users/akum1/diplom/доки/tz_llvm_frontend.md) — **техническое задание (главный spec)**: полное подмножество Solidity, архитектура компилятора, EVM intrinsics, storage layout, примеры трансляции, критерии готовности, научная новизна
- [`доки/scope_coursework.md`](/mnt/c/Users/akum1/diplom/доки/scope_coursework.md) — **план MVP на 8 недель (текущая задача)**: урезанное подмножество, понедельный план, демо-контракт Counter
- [`доки/setup_wsl.md`](/mnt/c/Users/akum1/diplom/доки/setup_wsl.md) — гайд по установке окружения (уже выполнен)
- [`доки/llvm_research.md`](/mnt/c/Users/akum1/diplom/доки/llvm_research.md) — образовательный разбор LLVM (reference)
- [`доки/edu/`](/mnt/c/Users/akum1/diplom/доки/edu/) — Hardhat-проект с примерами .sol контрактов
- [`доки/docs-ru/`](/mnt/c/Users/akum1/diplom/доки/docs-ru/) — русская документация Solidity

Читать их напрямую через `/mnt/c/...` — копировать в WSL не нужно.

---

## Текущий статус

**Курсовая работа (2 месяца, 8 недель) — MVP фронтенда:**
- [x] Этапы 0-4 setup_wsl.md — WSL + LLVM 18 + VSCode настроены
- [x] Этап 4.5 — первый bootstrap с IRBuilder (папка `hello-llvm/`)
- [ ] **Неделя 1** — структура `solidity-llvm/`, CMake, минимальный CLI
- [ ] Неделя 2 — лексер (ручной, ~300 строк)
- [ ] Неделя 3 — парсер: объявления
- [ ] Неделя 4 — парсер: statements + expressions
- [ ] Неделя 5 — семантический анализ
- [ ] Неделя 6 — storage layout + базовый codegen
- [ ] Неделя 7 — require, msg.sender, EVM intrinsics
- [ ] Неделя 8 — CLI, тесты, оформление пояснительной записки

**Следующий конкретный шаг:** создать структуру `~/diplom/solidity-llvm/` по плану недели 1 из `scope_coursework.md`.

---

## Стек и версии

| Компонент | Версия | Как подключается |
|---|---|---|
| C++ | C++20 | `set(CMAKE_CXX_STANDARD 20)` |
| CMake | 3.27+ | системный `apt install cmake` |
| Ninja | 1.11+ | `apt install ninja-build` |
| LLVM | **18.1.x** | `find_package(LLVM 18 REQUIRED CONFIG)` из `/usr/lib/llvm-18/cmake` |
| Clang | 18 | `apt install clang-18` (опционально как компилятор) |
| GoogleTest | 1.14+ | FetchContent или apt |
| cxxopts | 3.1+ | FetchContent (header-only) |
| nlohmann/json | 3.11+ | FetchContent или apt |
| spdlog | 1.14+ | FetchContent или apt |
| intx | 0.11+ | FetchContent (для возможной i256 арифметики в compile-time) |

**Важно:** vcpkg НЕ используем — на Ubuntu хватает системных пакетов + FetchContent для мелочей. В ТЗ vcpkg упоминается — это артефакт первоначального плана под Windows.

---

## Минимальное подмножество Solidity (MVP курсовой)

Жёстко ограничено, чтобы уложиться в 8 недель. Детали в `scope_coursework.md`.

**Типы:** только `uint256`, `bool`, `address`.
**Контракт:** один на файл, без наследования, без событий, без модификаторов, без конструктора.
**State variables:** только простые (без mapping/массивов/struct).
**Функции:** только `public`, без mutability-модификаторов.
**Statements:** `if/else`, `return`, `require(cond)`, объявление локальной переменной, присваивание.
**Выражения:** `+ - * /`, сравнения, `&& || !`, `msg.sender`, `msg.value`, литералы.

**Всё остальное** (mapping, массивы, for/while, внешние вызовы, модификаторы, наследование, events, int*, uint* ≠ 256) — выносится на диплом после курсовой.

---

## Демо-контракт курсовой

```solidity
contract Counter {
    uint256 public value;
    address public owner;

    function increment(uint256 delta) public returns (uint256) {
        require(msg.sender == owner);
        uint256 newValue = value + delta;
        value = newValue;
        return newValue;
    }

    function isOwner(address who) public returns (bool) {
        return who == owner;
    }
}
```

Критерий готовности курсовой: этот контракт компилируется в валидный `.ll`, проходящий `llvm-as` без ошибок.

---

## Правила взаимодействия

- **Язык общения:** русский. Пользователь — студент, готовит диплом.
- **Код:** C++20, комментарии минимальны, только где WHY неочевиден (LLVM особенности, EVM semantics, нетривиальные invariants). Не переводить LLVM API в комментариях.
- **Тесты:** GoogleTest, fixture-файлы `.sol` в `tests/fixtures/vulnerable/` и `tests/fixtures/safe/` (safe — для проверки что нет false positive).
- **Сборка:** всегда Ninja, Release по умолчанию для бенчмарков, Debug для разработки.

## Что НЕ делать

- Не расширять подмножество Solidity за пределы MVP без явной просьбы пользователя
- Не начинать LLVM passes (детекторы уязвимостей) — это фаза после курсовой
- Не пытаться запускать `i256` на x86 нативно — достаточно что `.ll` проходит `llvm-as`
- Не использовать vcpkg (Ubuntu = системные пакеты + FetchContent)
- Не копировать документы из `/mnt/c/...` в WSL — читать напрямую
- Не редактировать файлы в `/mnt/c/Users/akum1/diplom/llvm-project/` — это эталонный клон LLVM для справки

---

## Инструменты LLVM (доступны в `$PATH` после апдейта `.bashrc` из setup_wsl.md этап 2.5)

- `llvm-as input.ll -o output.bc` — валидация текстового IR → bitcode
- `llvm-dis input.bc -o output.ll` — обратно в текст
- `opt -O2 input.ll -o output.ll -S` — прогнать оптимизации
- `llc input.ll -o output.s` — кодогенерация в ассемблер (для i256 упадёт, и это нормально)
- `FileCheck` — матчинг ожидаемого IR в тестах (как в LLVM testsuite)

Подключение LLVM библиотек в CMake (шаблон):
```cmake
find_package(LLVM 18 REQUIRED CONFIG)
llvm_map_components_to_libnames(llvm_libs core support irreader bitwriter analysis)
target_include_directories(<target> PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(<target> PRIVATE ${llvm_libs})
```
