#include "solc/sema/Symbol.h"

namespace solc::sema {

bool Scope::add(Symbol s) {
    auto [it, ok] = table_.emplace(s.name, std::move(s));
    return ok;
}

Symbol* Scope::find(std::string_view name) {
    auto it = table_.find(std::string(name));
    return it == table_.end() ? nullptr : &it->second;
}

void SymbolTable::enterScope() { stack_.emplace_back(); }
void SymbolTable::leaveScope() { stack_.pop_back(); }

bool SymbolTable::declare(Symbol s) {
    return stack_.back().add(std::move(s));
}

Symbol* SymbolTable::lookup(std::string_view name) {
    for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
        if (Symbol* s = it->find(name)) return s;
    }
    return nullptr;
}

Symbol* SymbolTable::lookupCurrentScope(std::string_view name) {
    return stack_.back().find(name);
}

}  // namespace solc::sema
