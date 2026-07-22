# Week 1 — Skeleton, CMake, Stub CLI

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Создать структуру проекта `~/diplom/solidity-llvm/`, CMake-сборку с линковкой LLVM 18, минимальный CLI-стаб, каркас для GoogleTest — и доказать что всё это собирается и запускается.

**Architecture:** Sources under `src/`, headers under `include/solc/`, tests under `tests/`. Dependencies via `find_package(LLVM)` (система) + FetchContent (cxxopts, GoogleTest). На этой неделе мы не пишем компилятор — только рельсы, по которым он поедет.

**Tech Stack:** C++20, CMake ≥ 3.20, Ninja, LLVM 18.1, GoogleTest ≥ 1.14, cxxopts ≥ 3.1.

---

## Files

- Create: `~/diplom/solidity-llvm/CMakeLists.txt`
- Create: `~/diplom/solidity-llvm/cmake/FetchDeps.cmake`
- Create: `~/diplom/solidity-llvm/src/main.cpp`
- Create: `~/diplom/solidity-llvm/include/solc/driver/Driver.h`
- Create: `~/diplom/solidity-llvm/src/driver/Driver.cpp`
- Create: `~/diplom/solidity-llvm/tests/CMakeLists.txt`
- Create: `~/diplom/solidity-llvm/tests/test_smoke.cpp`
- Create: `~/diplom/solidity-llvm/.gitignore`
- Create: `~/diplom/solidity-llvm/README.md`

---

## Task 1: Инициализация git и базовая структура каталогов

- [ ] **Step 1.1: Создать каталоги**

```bash
cd ~/diplom
mkdir -p solidity-llvm/{src/driver,include/solc/driver,cmake,tests/fixtures,scripts}
cd solidity-llvm
git init -b main
```

- [ ] **Step 1.2: `.gitignore`**

Создать `.gitignore`:

```gitignore
build/
build-*/
.cache/
compile_commands.json
*.ll
*.bc
*.o
.vscode/
```

- [ ] **Step 1.3: Первый коммит**

```bash
git add .gitignore
git commit -m "chore: init repo with gitignore"
```

---

## Task 2: Минимальный CMakeLists, который находит LLVM

- [ ] **Step 2.1: Корневой `CMakeLists.txt`**

Создать `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(solidity_llvm CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(LLVM 18.1 REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")

include(cmake/FetchDeps.cmake)

add_subdirectory(src)

enable_testing()
add_subdirectory(tests)
```

- [ ] **Step 2.2: `cmake/FetchDeps.cmake` — cxxopts + GoogleTest**

Создать `cmake/FetchDeps.cmake`:

```cmake
include(FetchContent)

FetchContent_Declare(
    cxxopts
    GIT_REPOSITORY https://github.com/jarro2783/cxxopts.git
    GIT_TAG        v3.2.0
)
FetchContent_MakeAvailable(cxxopts)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
```

- [ ] **Step 2.3: Запустить CMake, убедиться что LLVM найден**

Run:

```bash
export PATH=/usr/lib/llvm-18/bin:$PATH
mkdir build && cd build
cmake .. -G Ninja
```

Expected: строка `-- Found LLVM 18.1.3 at /usr/lib/llvm-18/cmake` в выводе, без ошибок. Генерация упадёт на `add_subdirectory(src)` (нет файла) — это ок, сначала проверяем LLVM.

- [ ] **Step 2.4: Коммит**

```bash
cd ..
git add CMakeLists.txt cmake/FetchDeps.cmake
git commit -m "build: CMake skeleton with LLVM 18.1 and FetchContent deps"
```

---

## Task 3: Driver stub и main.cpp

- [ ] **Step 3.1: `include/solc/driver/Driver.h`**

```cpp
#pragma once
#include <string>
#include <string_view>

namespace solc {

struct CompileOptions {
    std::string inputPath;
    std::string outputPath;
    bool emitIR = false;
};

class Driver {
public:
    int run(const CompileOptions& opts);
};

}  // namespace solc
```

- [ ] **Step 3.2: `src/driver/Driver.cpp` — stub**

```cpp
#include "solc/driver/Driver.h"

#include <fstream>
#include <iostream>
#include <sstream>

namespace solc {

int Driver::run(const CompileOptions& opts) {
    std::ifstream in(opts.inputPath);
    if (!in) {
        std::cerr << "error: cannot open '" << opts.inputPath << "'\n";
        return 1;
    }
    std::stringstream buf;
    buf << in.rdbuf();
    const std::string source = buf.str();

    if (opts.emitIR) {
        // Неделя 1: ещё нечего эмитить. Пишем маркер.
        std::ostream* out = &std::cout;
        std::ofstream file;
        if (!opts.outputPath.empty()) {
            file.open(opts.outputPath);
            out = &file;
        }
        (*out) << "; solidity-llvm: stub output, source length="
               << source.size() << "\n";
    }
    return 0;
}

}  // namespace solc
```

- [ ] **Step 3.3: `src/main.cpp` с cxxopts**

```cpp
#include <cxxopts.hpp>
#include <iostream>

#include "solc/driver/Driver.h"

int main(int argc, char** argv) {
    cxxopts::Options opts("solidity-llvm",
                          "Solidity subset -> LLVM IR compiler (MVP)");
    opts.add_options()
        ("i,input",    "Input .sol file", cxxopts::value<std::string>())
        ("o,output",   "Output file (default stdout)",
             cxxopts::value<std::string>()->default_value(""))
        ("emit-ir",    "Emit LLVM IR")
        ("h,help",     "Print help");

    auto result = opts.parse(argc, argv);
    if (result.count("help") || !result.count("input")) {
        std::cout << opts.help() << '\n';
        return result.count("help") ? 0 : 1;
    }

    solc::CompileOptions co;
    co.inputPath  = result["input"].as<std::string>();
    co.outputPath = result["output"].as<std::string>();
    co.emitIR     = result.count("emit-ir") > 0;

    solc::Driver drv;
    return drv.run(co);
}
```

- [ ] **Step 3.4: `src/CMakeLists.txt`**

Создать `src/CMakeLists.txt`:

```cmake
add_library(solc_core STATIC
    driver/Driver.cpp
)
target_include_directories(solc_core
    PUBLIC ${CMAKE_SOURCE_DIR}/include
    PUBLIC ${LLVM_INCLUDE_DIRS}
)
target_compile_definitions(solc_core PUBLIC ${LLVM_DEFINITIONS})

llvm_map_components_to_libnames(llvm_libs core support irreader bitwriter analysis)
target_link_libraries(solc_core PUBLIC ${llvm_libs})

add_executable(solidity-llvm main.cpp)
target_link_libraries(solidity-llvm PRIVATE solc_core cxxopts::cxxopts)
```

- [ ] **Step 3.5: Собрать**

Run:

```bash
cd build
cmake .. -G Ninja
ninja solidity-llvm
```

Expected: бинарь `build/solidity-llvm` собран без ошибок.

- [ ] **Step 3.6: Smoke-запуск**

Run:

```bash
echo "contract X {}" > /tmp/x.sol
./solidity-llvm --input /tmp/x.sol --emit-ir
```

Expected: `; solidity-llvm: stub output, source length=14`.

- [ ] **Step 3.7: Коммит**

```bash
cd ..
git add src include
git commit -m "feat: stub CLI and Driver linked against LLVM"
```

---

## Task 4: GoogleTest-каркас

- [ ] **Step 4.1: Написать первый тест (failing)**

Создать `tests/test_smoke.cpp`:

```cpp
#include <gtest/gtest.h>

#include "solc/driver/Driver.h"

TEST(Smoke, DriverRejectsMissingFile) {
    solc::CompileOptions opts;
    opts.inputPath = "/no/such/file.sol";
    solc::Driver drv;
    EXPECT_NE(drv.run(opts), 0);
}

TEST(Smoke, DriverAcceptsExistingFile) {
    const char* path = "/tmp/solc_smoke.sol";
    {
        std::ofstream f(path);
        f << "contract X {}";
    }
    solc::CompileOptions opts;
    opts.inputPath = path;
    opts.emitIR    = true;
    solc::Driver drv;
    EXPECT_EQ(drv.run(opts), 0);
}
```

- [ ] **Step 4.2: `tests/CMakeLists.txt`**

```cmake
add_executable(unit_tests
    test_smoke.cpp
)
target_link_libraries(unit_tests PRIVATE
    solc_core
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(unit_tests)
```

Добавить `#include <fstream>` в `test_smoke.cpp` если ругается.

- [ ] **Step 4.3: Запустить тест, убедиться что зелёный**

Run:

```bash
cd build
cmake ..
ninja unit_tests
ctest --output-on-failure
```

Expected:

```
Test project /home/akum1/diplom/solidity-llvm/build
    Start 1: Smoke.DriverRejectsMissingFile
1/2 Test #1 ...   Passed
    Start 2: Smoke.DriverAcceptsExistingFile
2/2 Test #2 ...   Passed

100% tests passed, 0 tests failed out of 2
```

- [ ] **Step 4.4: Коммит**

```bash
cd ..
git add tests
git commit -m "test: smoke tests for Driver via GoogleTest"
```

---

## Task 5: README с инструкцией сборки

- [ ] **Step 5.1: `README.md`**

```markdown
# solidity-llvm

MVP-компилятор подмножества Solidity в LLVM IR. Курсовая работа, часть 1 диплома.

## Сборка

Требуется: Ubuntu 24.04 (или WSL), LLVM 18.1, CMake 3.20+, Ninja, git, C++20.

```bash
export PATH=/usr/lib/llvm-18/bin:$PATH
mkdir build && cd build
cmake .. -G Ninja
ninja
ctest --output-on-failure
```

## Запуск

```bash
./solidity-llvm --input path/to/file.sol --emit-ir --output out.ll
llvm-as out.ll
```

## Подмножество языка

См. `docs/scope.md` (копия из основной документации диплома).

## Статус

- [x] Week 1: skeleton + CMake + stub CLI
- [ ] Week 2: lexer
- [ ] ...
```

- [ ] **Step 5.2: Коммит**

```bash
git add README.md
git commit -m "docs: readme with build & run instructions"
```

---

## Task 6: Финальная верификация недели

- [ ] **Step 6.1: Чистая пересборка из нуля**

Run:

```bash
cd ~/diplom/solidity-llvm
rm -rf build
mkdir build && cd build
cmake .. -G Ninja
ninja
ctest --output-on-failure
./solidity-llvm --help
echo "contract Counter { uint256 x; }" > /tmp/c.sol
./solidity-llvm --input /tmp/c.sol --emit-ir
```

Expected:
- `ninja` без ошибок
- `ctest` — 2/2 Passed
- `--help` печатает описание опций
- `--input --emit-ir` выдаёт stub-строку

- [ ] **Step 6.2: Проверить `compile_commands.json` для IDE**

Run:

```bash
ls build/compile_commands.json
```

Expected: файл существует (для clangd/VSCode).

- [ ] **Step 6.3: Финальный коммит недели**

Если после шага 6.1 ничего не изменилось — коммит не нужен. Если были фиксы:

```bash
git add -u
git commit -m "build: week 1 complete — clean build and tests green"
```

---

## Критерий готовности недели 1

- [ ] Каталог `~/diplom/solidity-llvm/` существует и является git-репозиторием
- [ ] `ninja` собирает `solidity-llvm` и `unit_tests` без предупреждений
- [ ] `ctest` — 2/2 Passed
- [ ] `./solidity-llvm --help` работает
- [ ] LLVM 18.1 линкуется, `find_package` не жалуется
- [ ] Есть коммиты по Task 1–5 (минимум 4 штуки)
