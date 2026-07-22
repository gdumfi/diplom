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

## Запуск через Docker

Если LLVM 18 / CMake / Ninja ставить не хочется — есть готовый `Dockerfile`. На машине достаточно только Docker.

Собрать образ (один раз):

```bash
docker build -t solidity-llvm .
```

Скомпилировать встроенную демо-фикстуру (лежит в образе по пути `/opt/fixtures/`):

```bash
docker run --rm solidity-llvm --input /opt/fixtures/counter_no_require.sol --emit-ir
```

Скомпилировать свой `.sol` (примонтировать папку с файлами в `/work`):

```bash
docker run --rm -v "$PWD:/work" solidity-llvm \
    --input /work/contract.sol --emit-ir --output /work/contract.ll
```

Внутри образа также есть `llvm-as`, `llvm-dis`, `opt`, `llc` — можно валидировать IR прямо там:

```bash
docker run --rm --entrypoint llvm-as -v "$PWD:/work" solidity-llvm /work/contract.ll
```

