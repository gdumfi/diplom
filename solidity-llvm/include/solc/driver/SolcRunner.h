#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <llvm/Support/JSON.h>

namespace solc::driver {

// результат вызова solc --standard-json (через docker ethereum/solc)
struct SolcResult {
    bool ok = false;
    std::string error;                 // текст ошибки запуска/компиляции (при ok=false)
    llvm::json::Value root = nullptr;  // полный standard-json вывод (contracts + sources[].ast)
};

// скомпилировать исходник Solidity, вернуть разобранный standard-json.
// source   — текст контракта(ов); unitName — имя файла в AST (напр. "Vault.sol");
// solcVersion — тег образа ethereum/solc (по умолчанию 0.8.19).
// в settings.outputSelection запрашиваются ast + storageLayout.
SolcResult runSolc(const std::string& source, const std::string& unitName = "input.sol",
                   const std::string& solcVersion = "0.8.19");

// то же для НЕСКОЛЬКИХ файлов (multi-file контракты с импортами OpenZeppelin и т.п.):
// каждый файл — отдельная запись sources в standard-json, чтобы solc резолвил import-ы.
// remappings (@openzeppelin/=... solmate/=...) обязательны для контрактов с алиасами путей
SolcResult runSolcMulti(const std::vector<std::pair<std::string, std::string>>& sources,
                        const std::string& solcVersion = "0.8.19",
                        const std::vector<std::string>& remappings = {});

// построить индекс id -> узел AST (для резолва referencedDeclaration).
// ast — значение sources[unit].ast; индекс указывает внутрь него (жив, пока жив root).
void buildIdIndex(const llvm::json::Value& ast,
                  std::unordered_map<int64_t, const llvm::json::Object*>& index);

}  // namespace solc::driver
