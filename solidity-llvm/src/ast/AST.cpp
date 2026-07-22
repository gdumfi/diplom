#include "solc/ast/AST.h"

namespace solc::ast {

const char* typeKindName(TypeKind k) {
    switch (k) {
        case TypeKind::Uint256: return "uint256";
        case TypeKind::Bool:    return "bool";
        case TypeKind::Address: return "address";
        case TypeKind::Mapping: return "mapping";
    }
    return "?";
}

const char* binOpName(BinOp op) {
    switch (op) {
        case BinOp::Add:    return "+";
        case BinOp::Sub:    return "-";
        case BinOp::Mul:    return "*";
        case BinOp::Div:    return "/";
        case BinOp::Mod:    return "%";
        case BinOp::Eq:     return "==";
        case BinOp::NotEq:  return "!=";
        case BinOp::Lt:     return "<";
        case BinOp::Gt:     return ">";
        case BinOp::LtEq:   return "<=";
        case BinOp::GtEq:   return ">=";
        case BinOp::AndAnd: return "&&";
        case BinOp::OrOr:   return "||";
    }
    return "?";
}

const char* unOpName(UnOp op) {
    switch (op) {
        case UnOp::Not: return "!";
    }
    return "?";
}

void dump(const ContractDecl& c, std::ostream& os) {
    os << "contract " << c.name << " {\n";
    for (const auto& v : c.stateVars) {
        os << "  " << typeKindName(v.type.kind);
        if (v.isPublic) os << " public";
        os << ' ' << v.name << ";\n";
    }
    for (const auto& f : c.functions) {
        os << "  function " << f.name << "(";
        for (size_t i = 0; i < f.params.size(); ++i) {
            if (i) os << ", ";
            os << typeKindName(f.params[i].type.kind) << ' ' << f.params[i].name;
        }
        os << ") public";
        if (f.returnType) {
            os << " returns (" << typeKindName(f.returnType->kind) << ')';
        }
        os << " { ... }\n";
    }
    os << "}\n";
}

// --- глубокое копирование AST (для слияния наследования) ---

ExprPtr cloneExpr(const Expr& e) {
    ExprPtr out;
    switch (e.kind) {
        case ExprKind::NumberLit: {
            auto& n = static_cast<const NumberLit&>(e);
            out = std::make_unique<NumberLit>(n.text, n.loc);
            break;
        }
        case ExprKind::BoolLit: {
            auto& b = static_cast<const BoolLit&>(e);
            out = std::make_unique<BoolLit>(b.value, b.loc);
            break;
        }
        case ExprKind::Ident: {
            auto& id = static_cast<const IdentExpr&>(e);
            out = std::make_unique<IdentExpr>(id.name, id.loc);
            break;
        }
        case ExprKind::Binary: {
            auto& b = static_cast<const BinaryExpr&>(e);
            out = std::make_unique<BinaryExpr>(b.op, cloneExpr(*b.lhs), cloneExpr(*b.rhs), b.loc);
            break;
        }
        case ExprKind::Unary: {
            auto& u = static_cast<const UnaryExpr&>(e);
            out = std::make_unique<UnaryExpr>(u.op, cloneExpr(*u.operand), u.loc);
            break;
        }
        case ExprKind::Member: {
            auto& m = static_cast<const MemberExpr&>(e);
            out = std::make_unique<MemberExpr>(cloneExpr(*m.base), m.member, m.loc);
            break;
        }
        case ExprKind::Call: {
            auto& c = static_cast<const CallExpr&>(e);
            std::vector<ExprPtr> args;
            for (const auto& a : c.args) args.push_back(cloneExpr(*a));
            out = std::make_unique<CallExpr>(cloneExpr(*c.callee), std::move(args), c.loc);
            break;
        }
        case ExprKind::Index: {
            auto& ix = static_cast<const IndexExpr&>(e);
            out = std::make_unique<IndexExpr>(cloneExpr(*ix.base), cloneExpr(*ix.index), ix.loc);
            break;
        }
        case ExprKind::LowLevelCall: {
            auto& lc = static_cast<const LowLevelCallExpr&>(e);
            out = std::make_unique<LowLevelCallExpr>(
                cloneExpr(*lc.target), cloneExpr(*lc.value), lc.loc, lc.kind);
            break;
        }
        case ExprKind::Cond: {
            auto& ce = static_cast<const CondExpr&>(e);
            out = std::make_unique<CondExpr>(
                cloneExpr(*ce.cond), cloneExpr(*ce.thenE), cloneExpr(*ce.elseE), ce.loc);
            break;
        }
    }
    if (out) out->resolvedType = e.resolvedType;   // на всякий случай (обычно ещё пусто)
    return out;
}

StmtPtr cloneStmt(const Stmt& s) {
    switch (s.kind) {
        case StmtKind::VarDecl: {
            auto& v = static_cast<const VarDeclStmt&>(s);
            return std::make_unique<VarDeclStmt>(v.type, v.name, cloneExpr(*v.init), v.loc, v.inferred);
        }
        case StmtKind::Assign: {
            auto& a = static_cast<const AssignStmt&>(s);
            return std::make_unique<AssignStmt>(
                cloneExpr(*a.target), cloneExpr(*a.value), a.loc, a.compound);
        }
        case StmtKind::If: {
            auto& i = static_cast<const IfStmt&>(s);
            std::vector<StmtPtr> thenB, elseB;
            for (const auto& sp : i.thenBlock) thenB.push_back(cloneStmt(*sp));
            for (const auto& sp : i.elseBlock) elseB.push_back(cloneStmt(*sp));
            return std::make_unique<IfStmt>(cloneExpr(*i.cond), std::move(thenB), std::move(elseB), i.loc);
        }
        case StmtKind::Return: {
            auto& r = static_cast<const ReturnStmt&>(s);
            return std::make_unique<ReturnStmt>(r.value ? cloneExpr(*r.value) : nullptr, r.loc);
        }
        case StmtKind::Require: {
            auto& r = static_cast<const RequireStmt&>(s);
            return std::make_unique<RequireStmt>(cloneExpr(*r.cond), r.loc);
        }
        case StmtKind::Expr: {
            auto& es = static_cast<const ExprStmt&>(s);
            return std::make_unique<ExprStmt>(cloneExpr(*es.expr), es.loc);
        }
        case StmtKind::Throw:
            return std::make_unique<ThrowStmt>(s.loc);
        case StmtKind::Placeholder:
            return std::make_unique<PlaceholderStmt>(s.loc);
        case StmtKind::Selfdestruct: {
            auto& sd = static_cast<const SelfdestructStmt&>(s);
            return std::make_unique<SelfdestructStmt>(cloneExpr(*sd.recipient), sd.loc);
        }
    }
    return nullptr;
}

Block cloneBlock(const Block& b) {
    Block out;
    out.loc = b.loc;
    for (const auto& sp : b.stmts) out.stmts.push_back(cloneStmt(*sp));
    return out;
}

FunctionDecl cloneFunctionDecl(const FunctionDecl& fn) {
    FunctionDecl out;
    out.name       = fn.name;
    out.params     = fn.params;
    out.returnType = fn.returnType;
    out.modifiers  = fn.modifiers;
    out.body       = cloneBlock(fn.body);
    out.loc        = fn.loc;
    return out;
}

ModifierDecl cloneModifierDecl(const ModifierDecl& md) {
    ModifierDecl out;
    out.name   = md.name;
    out.params = md.params;
    out.body   = cloneBlock(md.body);
    out.loc    = md.loc;
    return out;
}

}  // namespace solc::ast
