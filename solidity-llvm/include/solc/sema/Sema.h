#pragma once
#include "solc/ast/AST.h"
#include "solc/diag/Diagnostic.h"
#include "solc/sema/Symbol.h"

namespace solc::sema {

class SemanticAnalyzer {
public:
    explicit SemanticAnalyzer(DiagnosticEngine& diag) : diag_(diag) {}

    bool analyze(ast::ContractDecl& contract);

private:
    bool registerContractMembers(ast::ContractDecl& c);
    bool analyzeFunction(ast::FunctionDecl& fn);
    bool analyzeModifier(ast::ModifierDecl& m);
    bool checkStmt(ast::Stmt& s);
    bool checkExpr(ast::Expr& e);

    DiagnosticEngine& diag_;
    SymbolTable       symbols_;
    const ast::Type*  currentReturnType_ = nullptr;
    // защита от переполнения стека на глубоких AST (длинные цепочки операторов)
    int               exprDepth_ = 0;
    static constexpr int kMaxExprDepth = 400;
};

}  // namespace solc::sema
