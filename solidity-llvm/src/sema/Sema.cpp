#include "solc/sema/Sema.h"

#include <string>

namespace solc::sema {

namespace {
bool sameType(const ast::Type& a, const ast::Type& b) {
    return a.kind == b.kind;
}
// выражения, семантику которых не моделируем: высокоуровневый вызов/каст
// (Type(x), obj.method(...), keccak256(...)) и member-доступ (поле struct, .length).
// их resolvedType — заглушка (uint256), поэтому строгую проверку типа при
// присваивании/возврате/var-decl к ним НЕ применяем — иначе ложная ошибка вида
// «Contract c = Contract(addr)» (address = opaque-uint256)
bool isOpaqueExpr(const ast::Expr& e) {
    return e.kind == ast::ExprKind::Call || e.kind == ast::ExprKind::Member;
}
}  // namespace

bool SemanticAnalyzer::registerContractMembers(ast::ContractDecl& c) {
    bool ok = true;
    int slot = 0;
    for (auto& sv : c.stateVars) {
        Symbol s{SymbolKind::StateVar, sv.name, sv.type, slot, sv.loc};
        if (!symbols_.declare(std::move(s))) {
            diag_.error(sv.loc, "duplicate state variable '" + sv.name + "'");
            ok = false;
        } else {
            sv.storageSlot = slot;
            slot++;
        }
    }
    for (auto& fn : c.functions) {
        ast::Type ftype = fn.returnType.value_or(ast::Type{ast::TypeKind::Uint256, {}});
        Symbol s{SymbolKind::Function, fn.name, ftype, -1, fn.loc};
        if (!symbols_.declare(std::move(s))) {
            diag_.error(fn.loc, "duplicate function '" + fn.name + "'");
            ok = false;
        }
    }
    return ok;
}

bool SemanticAnalyzer::checkExpr(ast::Expr& e) {
    using namespace ast;
    struct Guard { int& d; Guard(int& x) : d(x) { ++d; } ~Guard() { --d; } } g(exprDepth_);
    if (exprDepth_ > kMaxExprDepth) {
        diag_.error(e.loc, "expression too deeply nested");
        return false;
    }
    switch (e.kind) {
        case ExprKind::NumberLit:
            e.resolvedType = Type{TypeKind::Uint256, e.loc};
            return true;
        case ExprKind::BoolLit:
            e.resolvedType = Type{TypeKind::Bool, e.loc};
            return true;
        case ExprKind::Ident: {
            auto& id = static_cast<IdentExpr&>(e);
            // builtin now (block.timestamp) — глобальное символьное uint256
            if (id.name == "now") {
                id.resolvedKind = IdentExpr::Resolution::Builtin;
                id.resolvedType = Type{TypeKind::Uint256, id.loc};
                e.resolvedType  = *id.resolvedType;
                return true;
            }
            Symbol* sym = symbols_.lookup(id.name);
            if (!sym) {
                diag_.error(id.loc, "undeclared identifier '" + id.name + "'");
                return false;
            }
            // копируем нужное в AST по значению — указатели на Symbol небезопасны
            // после leaveScope(), а codegen на неделе 6 это читает.
            switch (sym->kind) {
                case SymbolKind::StateVar: id.resolvedKind = IdentExpr::Resolution::StateVar; break;
                case SymbolKind::Param:    id.resolvedKind = IdentExpr::Resolution::Param;    break;
                case SymbolKind::Local:    id.resolvedKind = IdentExpr::Resolution::Local;    break;
                case SymbolKind::Function: id.resolvedKind = IdentExpr::Resolution::Function; break;
            }
            id.resolvedSlot = sym->storageSlot;
            id.resolvedType = sym->type;
            return true;
        }
        case ExprKind::Binary: {
            auto& bin = static_cast<BinaryExpr&>(e);
            if (!checkExpr(*bin.lhs)) return false;
            if (!checkExpr(*bin.rhs)) return false;
            const Type& lt = *bin.lhs->resolvedType;
            const Type& rt = *bin.rhs->resolvedType;
            switch (bin.op) {
                case BinOp::Add: case BinOp::Sub:
                case BinOp::Mul: case BinOp::Div: case BinOp::Mod:
                    if (lt.kind != TypeKind::Uint256 || rt.kind != TypeKind::Uint256) {
                        diag_.error(bin.loc, std::string("operator '") + binOpName(bin.op)
                                                 + "' expects uint256");
                        return false;
                    }
                    e.resolvedType = Type{TypeKind::Uint256, bin.loc};
                    return true;
                case BinOp::Lt: case BinOp::Gt:
                case BinOp::LtEq: case BinOp::GtEq:
                    if (lt.kind != TypeKind::Uint256 || rt.kind != TypeKind::Uint256) {
                        diag_.error(bin.loc, std::string("operator '") + binOpName(bin.op)
                                                 + "' expects uint256");
                        return false;
                    }
                    e.resolvedType = Type{TypeKind::Bool, bin.loc};
                    return true;
                case BinOp::Eq: case BinOp::NotEq:
                    if (!sameType(lt, rt)) {
                        diag_.error(bin.loc, std::string("operator '") + binOpName(bin.op)
                                                 + "' requires operands of the same type");
                        return false;
                    }
                    e.resolvedType = Type{TypeKind::Bool, bin.loc};
                    return true;
                case BinOp::AndAnd: case BinOp::OrOr:
                    if (lt.kind != TypeKind::Bool || rt.kind != TypeKind::Bool) {
                        diag_.error(bin.loc, std::string("operator '") + binOpName(bin.op)
                                                 + "' expects bool");
                        return false;
                    }
                    e.resolvedType = Type{TypeKind::Bool, bin.loc};
                    return true;
            }
            return false;
        }
        case ExprKind::Unary: {
            auto& un = static_cast<UnaryExpr&>(e);
            if (!checkExpr(*un.operand)) return false;
            if (un.op == UnOp::Not) {
                if (un.operand->resolvedType->kind != TypeKind::Bool) {
                    diag_.error(un.loc, "operator '!' expects bool");
                    return false;
                }
                e.resolvedType = Type{TypeKind::Bool, un.loc};
                return true;
            }
            return false;
        }
        case ExprKind::Member: {
            auto& m = static_cast<MemberExpr&>(e);
            // спец-кейс: msg.sender / msg.value — builtin, не настоящий Symbol
            if (m.base->kind == ExprKind::Ident) {
                auto& base = static_cast<IdentExpr&>(*m.base);
                if (base.name == "msg") {
                    if (m.member == "sender") {
                        e.resolvedType = Type{TypeKind::Address, m.loc};
                        return true;
                    }
                    if (m.member == "value") {
                        e.resolvedType = Type{TypeKind::Uint256, m.loc};
                        return true;
                    }
                    diag_.error(m.loc, "unknown member 'msg." + m.member + "'");
                    return false;
                }
                if (base.name == "tx") {
                    if (m.member == "origin") {
                        e.resolvedType = Type{TypeKind::Address, m.loc};
                        return true;
                    }
                    diag_.error(m.loc, "unknown member 'tx." + m.member + "'");
                    return false;
                }
            }
            // прочий member-access (поле struct, .length, block.timestamp и т.п.) —
            // семантику агрегатов не моделируем, результат opaque (uint256)
            e.resolvedType = Type{TypeKind::Uint256, m.loc};
            return true;
        }
        case ExprKind::Call: {
            // высокоуровневый вызов obj.method(args) / каст Type(x) — моделируем как
            // opaque (uint256). НЕ считаем внешним вызовом (детектим только low-level
            // .call) — документированное упрощение. callee не проверяем, аргументы — да
            auto& c = static_cast<CallExpr&>(e);
            for (auto& a : c.args) if (!checkExpr(*a)) return false;
            e.resolvedType = Type{TypeKind::Uint256, e.loc};
            return true;
        }
        case ExprKind::Cond: {
            auto& ce = static_cast<CondExpr&>(e);
            if (!checkExpr(*ce.cond)) return false;
            if (ce.cond->resolvedType->kind != TypeKind::Bool) {
                diag_.error(ce.loc, "ternary condition must be bool");
                return false;
            }
            if (!checkExpr(*ce.thenE)) return false;
            if (!checkExpr(*ce.elseE)) return false;
            // тип результата = тип then-ветки (ветви обычно совпадают; агрегаты opaque)
            e.resolvedType = *ce.thenE->resolvedType;
            return true;
        }
        case ExprKind::Index: {
            auto& ix = static_cast<IndexExpr&>(e);
            if (!checkExpr(*ix.base)) return false;
            if (!checkExpr(*ix.index)) return false;
            const Type& bt = *ix.base->resolvedType;
            if (bt.kind == TypeKind::Mapping) {
                if (ix.index->resolvedType->kind != bt.mapKey) {
                    diag_.error(ix.loc, "mapping key type mismatch");
                    return false;
                }
                e.resolvedType = Type{bt.mapValue, ix.loc};   // тип значения mapping
            } else {
                // индексация массива/opaque — элемент opaque (uint256)
                e.resolvedType = Type{TypeKind::Uint256, ix.loc};
            }
            return true;
        }
        case ExprKind::LowLevelCall: {
            auto& lc = static_cast<LowLevelCallExpr&>(e);
            if (!checkExpr(*lc.target)) return false;
            if (lc.target->resolvedType->kind != TypeKind::Address) {
                diag_.error(lc.loc, "low-level call target must be an address");
                return false;
            }
            if (!checkExpr(*lc.value)) return false;
            if (lc.value->resolvedType->kind != TypeKind::Uint256) {
                diag_.error(lc.loc, "low-level call value must be uint256");
                return false;
            }
            // возвращает флаг успеха вызова
            e.resolvedType = Type{TypeKind::Bool, lc.loc};
            return true;
        }
        default:
            diag_.error(e.loc, "expression form not yet supported in sema");
            return false;
    }
}

bool SemanticAnalyzer::checkStmt(ast::Stmt& s) {
    using namespace ast;
    switch (s.kind) {
        case StmtKind::VarDecl: {
            auto& v = static_cast<VarDeclStmt&>(s);
            if (v.type.kind == TypeKind::Mapping) {
                diag_.error(v.loc, "mappings can only be state variables, not locals");
                return false;
            }
            if (!checkExpr(*v.init)) return false;
            // 'var name = init' — тип выводим из init (для агрегатов/opaque это
            // заглушка-uint256, чего достаточно: доступ к полям всё равно opaque)
            if (v.inferred) {
                v.type = *v.init->resolvedType;
                if (v.type.kind == TypeKind::Mapping) {
                    diag_.error(v.loc, "cannot infer 'var' of mapping type");
                    return false;
                }
            } else if (!isOpaqueExpr(*v.init) && v.init->resolvedType->kind != v.type.kind) {
                diag_.error(v.loc, "var-decl type mismatch");
                return false;
            }
            Symbol sym{SymbolKind::Local, v.name, v.type, -1, v.loc};
            if (!symbols_.declare(std::move(sym))) {
                diag_.error(v.loc, "duplicate local '" + v.name + "'");
                return false;
            }
            return true;
        }
        case StmtKind::Assign: {
            auto& a = static_cast<AssignStmt&>(s);
            // lvalue — простое имя (Ident) или элемент mapping (Index)
            if (!checkExpr(*a.target)) return false;
            if (a.target->kind == ExprKind::Ident) {
                auto& id = static_cast<IdentExpr&>(*a.target);
                if (id.resolvedKind == IdentExpr::Resolution::Function) {
                    diag_.error(a.loc, "cannot assign to a function");
                    return false;
                }
                if (a.target->resolvedType->kind == TypeKind::Mapping) {
                    diag_.error(a.loc, "cannot assign to a mapping as a whole");
                    return false;
                }
            }
            if (!checkExpr(*a.value)) return false;
            // строгую проверку типа применяем к скалярному имени и элементу mapping;
            // для поля struct (Member) и элемента массива/opaque (Index не-mapping)
            // тип не проверяем — семантику агрегатов не моделируем
            bool opaqueTarget =
                a.target->kind == ExprKind::Member ||
                (a.target->kind == ExprKind::Index &&
                 static_cast<IndexExpr&>(*a.target).base->resolvedType->kind != TypeKind::Mapping);
            // opaque и когда ЗНАЧЕНИЕ — немоделируемый каст/вызов (Contract c = Contract(addr))
            if (!opaqueTarget && !isOpaqueExpr(*a.value)) {
                if (a.value->resolvedType->kind != a.target->resolvedType->kind) {
                    diag_.error(a.loc, "assignment type mismatch");
                    return false;
                }
                if (a.compound && a.target->resolvedType->kind != TypeKind::Uint256) {
                    diag_.error(a.loc, "compound assignment '+=/-=' requires uint256");
                    return false;
                }
            }
            return true;
        }
        case StmtKind::Throw:
            // безусловный откат — проверять нечего
            return true;
        case StmtKind::Placeholder:
            // _; в теле модификатора — сюда подставится тело функции, проверять нечего
            return true;
        case StmtKind::Selfdestruct: {
            auto& sd = static_cast<SelfdestructStmt&>(s);
            if (!checkExpr(*sd.recipient)) return false;
            if (sd.recipient->resolvedType->kind != TypeKind::Address) {
                diag_.error(sd.loc, "selfdestruct expects an address recipient");
                return false;
            }
            return true;
        }
        case StmtKind::Require: {
            auto& r = static_cast<RequireStmt&>(s);
            if (!checkExpr(*r.cond)) return false;
            if (r.cond->resolvedType->kind != TypeKind::Bool) {
                diag_.error(r.loc, "require expects a bool condition");
                return false;
            }
            return true;
        }
        case StmtKind::Expr: {
            // выражение-инструкция: внешний вызов или безрезультатное выражение (no-op)
            auto& es = static_cast<ExprStmt&>(s);
            return checkExpr(*es.expr);
        }
        case StmtKind::If: {
            auto& i = static_cast<IfStmt&>(s);
            if (!checkExpr(*i.cond)) return false;
            if (i.cond->resolvedType->kind != TypeKind::Bool) {
                diag_.error(i.loc, "if condition must be bool");
                return false;
            }
            bool ok = true;
            symbols_.enterScope();
            for (auto& sp : i.thenBlock) if (!checkStmt(*sp)) ok = false;
            symbols_.leaveScope();
            symbols_.enterScope();
            for (auto& sp : i.elseBlock) if (!checkStmt(*sp)) ok = false;
            symbols_.leaveScope();
            return ok;
        }
        case StmtKind::Return: {
            auto& r = static_cast<ReturnStmt&>(s);
            if (!r.value) {
                if (currentReturnType_) {
                    diag_.error(r.loc, "function expects a return value");
                    return false;
                }
                return true;
            }
            if (!checkExpr(*r.value)) return false;
            if (!currentReturnType_) {
                diag_.error(r.loc, "function does not return a value");
                return false;
            }
            if (!isOpaqueExpr(*r.value) &&
                r.value->resolvedType->kind != currentReturnType_->kind) {
                diag_.error(r.loc, "return type mismatch");
                return false;
            }
            return true;
        }
        default:
            // safety net на случай новых StmtKind
            return true;
    }
}

bool SemanticAnalyzer::analyzeFunction(ast::FunctionDecl& fn) {
    symbols_.enterScope();
    bool ok = true;
    for (const auto& p : fn.params) {
        Symbol s{SymbolKind::Param, p.name, p.type, -1, p.loc};
        if (!symbols_.declare(std::move(s))) {
            diag_.error(p.loc, "duplicate parameter '" + p.name + "'");
            ok = false;
        }
    }
    currentReturnType_ = fn.returnType ? &*fn.returnType : nullptr;
    for (auto& sp : fn.body.stmts) {
        if (!checkStmt(*sp)) ok = false;
    }
    currentReturnType_ = nullptr;
    symbols_.leaveScope();
    return ok;
}

bool SemanticAnalyzer::analyzeModifier(ast::ModifierDecl& m) {
    symbols_.enterScope();
    bool ok = true;
    for (const auto& p : m.params) {
        Symbol s{SymbolKind::Param, p.name, p.type, -1, p.loc};
        if (!symbols_.declare(std::move(s))) {
            diag_.error(p.loc, "duplicate parameter '" + p.name + "'");
            ok = false;
        }
    }
    // тело модификатора живёт в контексте контракта: state vars (owner) уже видны
    currentReturnType_ = nullptr;
    for (auto& sp : m.body.stmts) {
        if (!checkStmt(*sp)) ok = false;
    }
    symbols_.leaveScope();
    return ok;
}

bool SemanticAnalyzer::analyze(ast::ContractDecl& c) {
    symbols_.enterScope();
    bool ok = registerContractMembers(c);
    // модификаторы анализируем в области контракта (до функций)
    for (auto& m : c.modifiers) {
        if (!analyzeModifier(m)) ok = false;
    }
    for (auto& fn : c.functions) {
        if (!analyzeFunction(fn)) ok = false;
    }
    symbols_.leaveScope();
    return ok && !diag_.hasErrors();
}

}  // namespace solc::sema
