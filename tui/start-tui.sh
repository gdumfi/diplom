#!/usr/bin/env bash
# Запуск SCSA TUI из WSL. Файловые сканы (.sol) работают без ключа.
# Для скана по адресу заранее: export ETHERSCAN_API_KEY=...
export SCA_BIN="${SCA_BIN:-$HOME/diplom/solidity-llvm/build/src/solidity-llvm}"
exec "$HOME/diplom/tui/scsa-tui"
