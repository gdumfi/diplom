#pragma once
#include <vector>

#include "solc/ast/AST.h"
#include "solc/diag/Diagnostic.h"

namespace solc::sema {

// слияние наследования на уровне AST (пост-проход между парсингом и sema).
// для контрактов с непустым baseContracts, базы которых определены В ТОМ ЖЕ файле,
// сливает унаследованные state-vars (base-first, дедуп по имени — база выигрывает
// слот), функции и модификаторы (derived override) в «плоский» контракт. импорты
// (база вне файла) и циклы наследования безопасно пропускаются.
//
// ИНВАРИАНТ 0-РЕГРЕССА: если ни у одного контракта нет базы — возвращает вход как
// есть (identity). слоты НЕ проставляет — нумерует sema по финальному порядку.
std::vector<ast::ContractDecl> mergeInheritance(std::vector<ast::ContractDecl> contracts,
                                                DiagnosticEngine& diag);

}  // namespace solc::sema
