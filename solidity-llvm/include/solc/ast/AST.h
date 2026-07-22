#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "solc/diag/Diagnostic.h"

namespace solc::ast {

enum class TypeKind { Uint256, Bool, Address, Mapping };

struct Type {
    TypeKind  kind = TypeKind::Uint256;
    SourceLoc loc;
    // для Mapping: типы ключа и значения (в MVP только простые — address=>uint256).
    // осмысленны только при kind == Mapping. стоят ПОСЛЕ loc с дефолтами, чтобы
    // существующие агрегатные Type{kind, loc} продолжали работать
    TypeKind  mapKey   = TypeKind::Address;
    TypeKind  mapValue = TypeKind::Uint256;
};

enum class ExprKind { NumberLit, BoolLit, Ident, Binary, Unary, Member, Call, LowLevelCall, Index, Cond };

struct Expr {
    ExprKind             kind;
    SourceLoc            loc;
    std::optional<Type>  resolvedType;
    Expr(ExprKind k, SourceLoc l) : kind(k), loc(l) {}
    virtual ~Expr() = default;
};
using ExprPtr = std::unique_ptr<Expr>;

struct NumberLit : Expr {
    std::string text;
    NumberLit(std::string t, SourceLoc l)
        : Expr(ExprKind::NumberLit, l), text(std::move(t)) {}
};

struct BoolLit : Expr {
    bool value;
    BoolLit(bool v, SourceLoc l) : Expr(ExprKind::BoolLit, l), value(v) {}
};

struct IdentExpr : Expr {
    std::string name;
    enum class Resolution { Unresolved, StateVar, Param, Local, Function, Builtin };
    Resolution resolvedKind = Resolution::Unresolved;
    int        resolvedSlot = -1;   // storageSlot для StateVar; иначе -1
    IdentExpr(std::string n, SourceLoc l)
        : Expr(ExprKind::Ident, l), name(std::move(n)) {}
};

enum class BinOp {
    Add, Sub, Mul, Div, Mod,
    Eq, NotEq, Lt, Gt, LtEq, GtEq,
    AndAnd, OrOr,
};

struct BinaryExpr : Expr {
    BinOp   op;
    ExprPtr lhs;
    ExprPtr rhs;
    BinaryExpr(BinOp o, ExprPtr l, ExprPtr r, SourceLoc loc)
        : Expr(ExprKind::Binary, loc),
          op(o), lhs(std::move(l)), rhs(std::move(r)) {}
};

enum class UnOp { Not };

struct UnaryExpr : Expr {
    UnOp    op;
    ExprPtr operand;
    UnaryExpr(UnOp o, ExprPtr e, SourceLoc loc)
        : Expr(ExprKind::Unary, loc), op(o), operand(std::move(e)) {}
};

struct MemberExpr : Expr {
    ExprPtr     base;
    std::string member;
    MemberExpr(ExprPtr b, std::string m, SourceLoc loc)
        : Expr(ExprKind::Member, loc),
          base(std::move(b)), member(std::move(m)) {}
};

struct CallExpr : Expr {
    ExprPtr              callee;
    std::vector<ExprPtr> args;
    CallExpr(ExprPtr c, std::vector<ExprPtr> a, SourceLoc loc)
        : Expr(ExprKind::Call, loc),
          callee(std::move(c)), args(std::move(a)) {}
};

// индексация mapping: base[index], напр. balances[msg.sender].
// используется и как rvalue (чтение), и как lvalue (присваивание)
struct IndexExpr : Expr {
    ExprPtr base;    // mapping-переменная (IdentExpr со StateVar)
    ExprPtr index;   // ключ
    IndexExpr(ExprPtr b, ExprPtr i, SourceLoc loc)
        : Expr(ExprKind::Index, loc), base(std::move(b)), index(std::move(i)) {}
};

// тернарный выбор: cond ? thenE : elseE. семантику агрегатов не моделируем —
// тип результата берём из then-ветки; в codegen — llvm select
struct CondExpr : Expr {
    ExprPtr cond;
    ExprPtr thenE;
    ExprPtr elseE;
    CondExpr(ExprPtr c, ExprPtr t, ExprPtr e, SourceLoc loc)
        : Expr(ExprKind::Cond, loc),
          cond(std::move(c)), thenE(std::move(t)), elseE(std::move(e)) {}
};

// вид низкоуровневого вызова: call (перевод value) или delegatecall (чужой код
// в контексте текущего контракта — особо опасен, SWC-112)
enum class LowLevelKind { Call, DelegateCall };

// низкоуровневый внешний вызов: target.call{value: value}("") / target.delegatecall(...)
// это точка передачи управления недоверенному коду — ядро анализа reentrancy.
// calldata-аргумент в MVP всегда "" и не хранится
struct LowLevelCallExpr : Expr {
    ExprPtr       target;   // адрес получателя (напр. msg.sender)
    ExprPtr       value;    // передаваемое количество wei (для delegatecall — nullptr)
    LowLevelKind  kind = LowLevelKind::Call;
    LowLevelCallExpr(ExprPtr t, ExprPtr v, SourceLoc loc, LowLevelKind k = LowLevelKind::Call)
        : Expr(ExprKind::LowLevelCall, loc),
          target(std::move(t)), value(std::move(v)), kind(k) {}
};

const char* binOpName(BinOp op);
const char* unOpName(UnOp op);

enum class StmtKind { VarDecl, Assign, If, Return, Require, Expr, Throw, Placeholder, Selfdestruct };

struct Stmt {
    StmtKind  kind;
    SourceLoc loc;
    Stmt(StmtKind k, SourceLoc l) : kind(k), loc(l) {}
    virtual ~Stmt() = default;
};
using StmtPtr = std::unique_ptr<Stmt>;

struct VarDeclStmt : Stmt {
    Type        type;
    std::string name;
    ExprPtr     init;
    // объявление через 'var' (без явного типа) — тип выводится в sema из init.
    // codegen тогда берёт тип из init, а не из заглушки-type
    bool        inferred = false;
    VarDeclStmt(Type t, std::string n, ExprPtr i, SourceLoc loc, bool inf = false)
        : Stmt(StmtKind::VarDecl, loc),
          type(t), name(std::move(n)), init(std::move(i)), inferred(inf) {}
};

// присваивание lvalue = value. target — lvalue-выражение: IdentExpr (простая
// переменная) либо IndexExpr (элемент mapping, balances[k] = ...).
// compound: для составных операций (+=, -=) хранит операцию; nullopt — обычное '='
struct AssignStmt : Stmt {
    ExprPtr                target;
    ExprPtr                value;
    std::optional<BinOp>   compound;   // Add для +=, Sub для -=, nullopt для =
    AssignStmt(ExprPtr t, ExprPtr v, SourceLoc loc, std::optional<BinOp> comp = std::nullopt)
        : Stmt(StmtKind::Assign, loc),
          target(std::move(t)), value(std::move(v)), compound(comp) {}
};

struct IfStmt : Stmt {
    ExprPtr               cond;
    std::vector<StmtPtr>  thenBlock;
    std::vector<StmtPtr>  elseBlock;
    IfStmt(ExprPtr c, std::vector<StmtPtr> t,
           std::vector<StmtPtr> e, SourceLoc loc)
        : Stmt(StmtKind::If, loc),
          cond(std::move(c)),
          thenBlock(std::move(t)),
          elseBlock(std::move(e)) {}
};

struct ReturnStmt : Stmt {
    ExprPtr value;
    ReturnStmt(ExprPtr v, SourceLoc loc)
        : Stmt(StmtKind::Return, loc), value(std::move(v)) {}
};

struct RequireStmt : Stmt {
    ExprPtr cond;
    RequireStmt(ExprPtr c, SourceLoc loc)
        : Stmt(StmtKind::Require, loc), cond(std::move(c)) {}
};

// выражение-инструкция: в MVP допускается только для внешнего вызова
// (target.call{...}(""); ), результат которого не используется
struct ExprStmt : Stmt {
    ExprPtr expr;
    ExprStmt(ExprPtr e, SourceLoc loc)
        : Stmt(StmtKind::Expr, loc), expr(std::move(e)) {}
};

// throw; (старый Solidity) — безусловный откат транзакции, как revert
struct ThrowStmt : Stmt {
    explicit ThrowStmt(SourceLoc loc) : Stmt(StmtKind::Throw, loc) {}
};

// _;  — placeholder в теле модификатора: сюда подставляется тело функции
struct PlaceholderStmt : Stmt {
    explicit PlaceholderStmt(SourceLoc loc) : Stmt(StmtKind::Placeholder, loc) {}
};

// selfdestruct(recipient) — уничтожает контракт, шлёт остаток recipient.
// без проверки прав — критическая уязвимость (SWC-106, ср. Parity)
struct SelfdestructStmt : Stmt {
    ExprPtr recipient;
    SelfdestructStmt(ExprPtr r, SourceLoc loc)
        : Stmt(StmtKind::Selfdestruct, loc), recipient(std::move(r)) {}
};

struct Param {
    Type        type;
    std::string name;
    SourceLoc   loc;
};

struct StateVarDecl {
    Type        type;
    std::string name;
    bool        isPublic = false;
    int         storageSlot = -1;
    SourceLoc   loc;
};

struct Block {
    std::vector<StmtPtr> stmts;
    SourceLoc            loc;
};

struct FunctionDecl {
    std::string               name;
    std::vector<Param>        params;
    std::optional<Type>       returnType;
    std::vector<std::string>  modifiers;   // применённые модификаторы (по имени)
    Block                     body;
    SourceLoc                 loc;
};

// определение модификатора: modifier name([params]) { body с _; }
struct ModifierDecl {
    std::string        name;
    std::vector<Param> params;
    Block              body;
    SourceLoc          loc;
};

struct ContractDecl {
    std::string                 name;
    std::vector<StateVarDecl>   stateVars;
    std::vector<ModifierDecl>   modifiers;
    std::vector<FunctionDecl>   functions;
    // имена базовых контрактов из клаузы `is Base1, Base2` (слева направо). пусто —
    // наследования нет. используется пост-проходом слияния (sema/Inheritance)
    std::vector<std::string>    baseContracts;
    SourceLoc                   loc;
};

// печать в человекочитаемом виде — для отладочного --ast-dump
void dump(const ContractDecl& c, std::ostream& os);

const char* typeKindName(TypeKind k);

// глубокое копирование AST-узлов (Block содержит unique_ptr — тривиальный copy нельзя).
// нужно для слияния наследования: члены базы клонируются в производный контракт
ExprPtr      cloneExpr(const Expr& e);
StmtPtr      cloneStmt(const Stmt& s);
Block        cloneBlock(const Block& b);
FunctionDecl cloneFunctionDecl(const FunctionDecl& fn);
ModifierDecl cloneModifierDecl(const ModifierDecl& md);

}  // namespace solc::ast
