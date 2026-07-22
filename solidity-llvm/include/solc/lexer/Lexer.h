#pragma once
#include <string_view>
#include "solc/lexer/Token.h"
#include "solc/diag/Diagnostic.h"

namespace solc {

class Lexer {
public:
    Lexer(std::string_view source, DiagnosticEngine& diag)
        : src_(source), diag_(diag) {}

    Token next();

private:
    char peek(size_t offset = 0) const;
    char advance();
    bool match(char c);
    void skipWhitespaceAndComments();

    Token makeToken(TokenKind k, size_t start, SourceLoc startLoc) const;
    Token lexIdentifierOrKeyword(size_t start, SourceLoc startLoc);
    Token lexNumber(size_t start, SourceLoc startLoc);
    Token lexString(size_t start, SourceLoc startLoc);

    std::string_view  src_;
    DiagnosticEngine& diag_;
    size_t    pos_  = 0;
    SourceLoc loc_{1, 1};
};

}  // namespace solc
