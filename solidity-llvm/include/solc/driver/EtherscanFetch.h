#pragma once
#include <string>
#include <utility>
#include <vector>

namespace solc::driver {

// тянет верифицированный исходник по адресу с Etherscan V2 API (единый по chainId).
// ключ — из env ETHERSCAN_API_KEY (как RPC для форка, не хардкодим). обрабатывает и
// плоский исходник, и multi-file standard-json ({{...}}) — возвращает КАЖДЫЙ файл
// отдельно (путь→контент), чтобы solc резолвил import-ы (OpenZeppelin и т.п.).
// contractName — имя главного контракта; compilerVersion — версия solc из Etherscan
// (напр. "0.8.26", чтобы взять точный образ); files — все файлы; err — причина.
// возвращает true при успехе. требует curl+python3.
bool fetchVerifiedSources(const std::string& address, int chainId,
                          std::string& contractName, std::string& compilerVersion,
                          std::vector<std::pair<std::string, std::string>>& files,
                          std::vector<std::string>& remappings,
                          std::string& err);

}  // namespace solc::driver
