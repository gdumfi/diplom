#pragma once
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "solc/ast/AST.h"
#include "solc/diag/Diagnostic.h"

namespace solc::sema {

enum class SymbolKind { StateVar, Param, Local, Function };

struct Symbol {
    SymbolKind   kind;
    std::string  name;
    ast::Type    type;
    int          storageSlot = -1;   // только для StateVar
    SourceLoc    declLoc;
};

class Scope {
public:
    bool    add(Symbol s);
    Symbol* find(std::string_view name);
private:
    std::unordered_map<std::string, Symbol> table_;
};

class SymbolTable {
public:
    void    enterScope();
    void    leaveScope();
    bool    declare(Symbol s);
    Symbol* lookup(std::string_view name);
    Symbol* lookupCurrentScope(std::string_view name);
private:
    std::vector<Scope> stack_;
};

}  // namespace solc::sema
