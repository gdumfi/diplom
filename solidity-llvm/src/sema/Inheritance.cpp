#include "solc/sema/Inheritance.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <set>
#include <unordered_map>

namespace solc::sema {

std::vector<ast::ContractDecl> mergeInheritance(std::vector<ast::ContractDecl> contracts,
                                                DiagnosticEngine& /*diag*/) {
    // гейт 0-регресса: нет ни одного наследования → возвращаем как есть (identity)
    bool any = false;
    for (const auto& c : contracts)
        if (!c.baseContracts.empty()) { any = true; break; }
    if (!any) return contracts;

    // индекс по имени (форвард-референс: база может стоять ниже derived по файлу)
    std::unordered_map<std::string, const ast::ContractDecl*> byName;
    for (const auto& c : contracts) byName[c.name] = &c;

    // линеаризация предков: base-first, защита от циклов/повторов через visited.
    // добавляет в out предков контракта name (без него самого), базовейшие первыми
    std::function<void(const std::string&, std::vector<const ast::ContractDecl*>&,
                       std::set<std::string>&)> collect =
        [&](const std::string& name, std::vector<const ast::ContractDecl*>& out,
            std::set<std::string>& visited) {
            auto it = byName.find(name);
            if (it == byName.end()) return;   // база вне файла (импорт) — безопасно пропускаем
            for (const auto& base : it->second->baseContracts) {
                if (visited.count(base)) continue;   // цикл/diamond-повтор — пропускаем ветку
                visited.insert(base);
                collect(base, out, visited);         // сначала предки базы
                auto bit = byName.find(base);
                if (bit != byName.end()) out.push_back(bit->second);   // потом сама база
            }
        };

    // ПРОХОД 1: строим плоские версии, читая базы через byName (contracts НЕ трогаем —
    // иначе указатели byName повиснут на moved-from объектах)
    std::vector<std::optional<ast::ContractDecl>> flats(contracts.size());
    for (size_t idx = 0; idx < contracts.size(); ++idx) {
        auto& c = contracts[idx];
        if (c.baseContracts.empty()) continue;

        std::vector<const ast::ContractDecl*> ancestors;   // base-first
        std::set<std::string> visited{c.name};             // не включаем себя
        collect(c.name, ancestors, visited);

        ast::ContractDecl flat;
        flat.name = c.name;
        flat.loc  = c.loc;

        std::set<std::string> svNames, modNames, fnNames;
        // state-vars: base-first, дедуп по имени (первое появление = база выигрывает СЛОТ;
        // derived-инициализатор shadowing отбрасываем). порядок задаёт нумерацию слотов sema
        auto addStateVars = [&](const std::vector<ast::StateVarDecl>& svs) {
            for (const auto& sv : svs)
                if (svNames.insert(sv.name).second) flat.stateVars.push_back(sv);
        };
        for (const auto* anc : ancestors) addStateVars(anc->stateVars);
        addStateVars(c.stateVars);

        // функции/модификаторы: derived ОVERRIDE — сначала собственные c, затем предки
        // от более-производного к базовому (ancestors base-first → идём в обратном порядке)
        auto addMods = [&](const std::vector<ast::ModifierDecl>& mods) {
            for (const auto& md : mods)
                if (modNames.insert(md.name).second)
                    flat.modifiers.push_back(ast::cloneModifierDecl(md));
        };
        auto addFns = [&](const std::vector<ast::FunctionDecl>& fns) {
            for (const auto& fn : fns)
                if (fnNames.insert(fn.name).second)
                    flat.functions.push_back(ast::cloneFunctionDecl(fn));
        };
        addMods(c.modifiers);
        addFns(c.functions);
        for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
            addMods((*it)->modifiers);
            addFns((*it)->functions);
        }

        flats[idx] = std::move(flat);
    }

    // ПРОХОД 2: собираем результат (плоские версии там, где были базы; иначе оригинал)
    std::vector<ast::ContractDecl> result;
    result.reserve(contracts.size());
    for (size_t idx = 0; idx < contracts.size(); ++idx) {
        if (flats[idx]) result.push_back(std::move(*flats[idx]));
        else            result.push_back(std::move(contracts[idx]));
    }
    return result;
}

}  // namespace solc::sema
