#pragma once
#include <optional>
#include <string_view>

#include "solc/ast/AST.h"
#include "solc/diag/Diagnostic.h"
#include "solc/lexer/Lexer.h"
#include "solc/lexer/Token.h"

namespace solc {

class Parser {
public:
    Parser(Lexer& lex, DiagnosticEngine& diag);

    // верхний уровень: все контракты файла (пропускает pragma/import,
    // interface/library-объявления не анализируем — пропускаем)
    std::vector<ast::ContractDecl> parseSourceUnit();

    // один контракт
    std::optional<ast::ContractDecl> parseContract();

    // публично ради unit-тестируемости отдельных продукций
    std::optional<ast::Type> parseType();
    std::optional<ast::TypeKind> parseSimpleTypeKind();   // uint256/bool/address (ключ/значение mapping)
    std::optional<ast::StateVarDecl> parseStateVarDecl();
    std::optional<ast::FunctionDecl> parseFunctionDecl();
    std::optional<ast::ModifierDecl> parseModifierDecl();

    ast::ExprPtr parseExpr();
    ast::StmtPtr parseStmt();

private:
    ast::ExprPtr parsePrimary();
    ast::ExprPtr parseUnary();
    ast::ExprPtr parseBinary(int minPrec);

    static int                       binOpPrecedence(TokenKind k);
    static std::optional<ast::BinOp> tokenToBinOp(TokenKind k);


    const Token& peek() const { return current_; }
    Token        advance();              // продвигаемся, возвращаем потреблённый
    bool         check(TokenKind k) const { return current_.kind == k; }
    bool         match(TokenKind k);     // если совпало — потребить и вернуть true
    bool         expect(TokenKind k, std::string_view ctx); // иначе diag, вернуть false

    std::optional<ast::Param> parseParam();
    bool parseBlockStatements(std::vector<ast::StmtPtr>& out);
    bool parseBlockOrStatement(std::vector<ast::StmtPtr>& out);  // { ... } либо один statement
    void skipBraceBody();   // потребляет от текущего '{' до парного '}'
    void skipPragma();      // пропускает директиву pragma ... ;
    bool skipParenGroup();  // потребляет сбалансированную группу от '(' до парной ')'

    // продвинуться до ';' включительно или до конца блока/файла
    void synchronizeToSemicolon();
    // продвинуться до конца тела функции (потребляет '{' и парный '}')
    void synchronizeToBraceEnd();

    // ограничитель глубины рекурсивного спуска — защита от переполнения стека
    // на враждебно/патологически вложенном вводе (((...)))/!!!.../вложенные if
    int               depth_ = 0;
    static constexpr int kMaxDepth = 400;

    Lexer&            lex_;
    DiagnosticEngine& diag_;
    Token             current_;          // 1-токен lookahead кэш
};

}  // namespace solc
