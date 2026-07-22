#include "solc/lexer/Lexer.h"
#include <cctype>

namespace solc {

const char* tokenKindName(TokenKind k) {
    switch (k) {
#define CASE(x) case TokenKind::x: return #x;
        CASE(Identifier) CASE(Number) CASE(True) CASE(False) CASE(StringLit)
        CASE(KwUint256) CASE(KwBool) CASE(KwAddress)
        CASE(KwContract) CASE(KwFunction) CASE(KwPublic)
        CASE(KwReturns) CASE(KwReturn) CASE(KwIf) CASE(KwElse) CASE(KwRequire)
        CASE(KwMapping) CASE(KwThrow) CASE(KwPragma) CASE(KwModifier)
        CASE(Plus) CASE(Minus) CASE(Star) CASE(Slash) CASE(Percent) CASE(Caret)
        CASE(Eq) CASE(EqEq) CASE(NotEq) CASE(Lt) CASE(Gt) CASE(LtEq) CASE(GtEq)
        CASE(PlusEq) CASE(MinusEq)
        CASE(AndAnd) CASE(OrOr) CASE(Not) CASE(FatArrow)
        CASE(LParen) CASE(RParen) CASE(LBrace) CASE(RBrace)
        CASE(LBracket) CASE(RBracket)
        CASE(Comma) CASE(Semicolon) CASE(Dot) CASE(Colon) CASE(Question)
        CASE(EndOfFile) CASE(Invalid)
#undef CASE
    }
    return "?";
}

char Lexer::peek(size_t offset) const {
    return pos_ + offset < src_.size() ? src_[pos_ + offset] : '\0';
}

char Lexer::advance() {
    char c = src_[pos_++];
    if (c == '\n') { loc_.line++; loc_.col = 1; }
    else           { loc_.col++; }
    return c;
}

bool Lexer::match(char c) {
    if (peek() != c) return false;
    advance();
    return true;
}

void Lexer::skipWhitespaceAndComments() {
    for (;;) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
        } else if (c == '/' && peek(1) == '/') {
            while (peek() != '\n' && peek() != '\0') advance();
        } else if (c == '/' && peek(1) == '*') {
            SourceLoc commentStart = loc_;
            advance(); advance();
            while (peek() != '\0' && !(peek() == '*' && peek(1) == '/')) advance();
            if (peek() == '*') {
                advance(); advance();
            } else {
                // дошли до EOF без закрывающего */
                diag_.error(commentStart, "unterminated block comment");
            }
        } else {
            return;
        }
    }
}

Token Lexer::makeToken(TokenKind k, size_t start, SourceLoc startLoc) const {
    return Token{k, src_.substr(start, pos_ - start), startLoc};
}

Token Lexer::next() {
    skipWhitespaceAndComments();
    if (pos_ >= src_.size()) return Token{TokenKind::EndOfFile, {}, loc_};

    SourceLoc startLoc = loc_;
    size_t    start    = pos_;
    char      c        = advance();

    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
        return lexIdentifierOrKeyword(start, startLoc);
    }
    if (std::isdigit(static_cast<unsigned char>(c))) {
        return lexNumber(start, startLoc);
    }

    if (c == '"') return lexString(start, startLoc);

    switch (c) {
        case '(': return makeToken(TokenKind::LParen,    start, startLoc);
        case ')': return makeToken(TokenKind::RParen,    start, startLoc);
        case '{': return makeToken(TokenKind::LBrace,    start, startLoc);
        case '}': return makeToken(TokenKind::RBrace,    start, startLoc);
        case ',': return makeToken(TokenKind::Comma,     start, startLoc);
        case ';': return makeToken(TokenKind::Semicolon, start, startLoc);
        case '.': return makeToken(TokenKind::Dot,       start, startLoc);
        case ':': return makeToken(TokenKind::Colon,     start, startLoc);
        case '?': return makeToken(TokenKind::Question,  start, startLoc);
        case '[': return makeToken(TokenKind::LBracket,  start, startLoc);
        case ']': return makeToken(TokenKind::RBracket,  start, startLoc);
        case '+':
            if (match('=')) return makeToken(TokenKind::PlusEq,  start, startLoc);
            return makeToken(TokenKind::Plus,      start, startLoc);
        case '-':
            if (match('=')) return makeToken(TokenKind::MinusEq, start, startLoc);
            return makeToken(TokenKind::Minus,     start, startLoc);
        case '*': return makeToken(TokenKind::Star,      start, startLoc);
        case '/': return makeToken(TokenKind::Slash,     start, startLoc);
        case '%': return makeToken(TokenKind::Percent,   start, startLoc);
        case '^': return makeToken(TokenKind::Caret,     start, startLoc);

        case '=':
            if (match('=')) return makeToken(TokenKind::EqEq,     start, startLoc);
            if (match('>')) return makeToken(TokenKind::FatArrow, start, startLoc);
            return makeToken(TokenKind::Eq, start, startLoc);
        case '!':
            if (match('=')) return makeToken(TokenKind::NotEq, start, startLoc);
            return makeToken(TokenKind::Not, start, startLoc);
        case '<':
            if (match('=')) return makeToken(TokenKind::LtEq,  start, startLoc);
            return makeToken(TokenKind::Lt, start, startLoc);
        case '>':
            if (match('=')) return makeToken(TokenKind::GtEq,  start, startLoc);
            return makeToken(TokenKind::Gt, start, startLoc);
        case '&':
            if (match('&')) return makeToken(TokenKind::AndAnd, start, startLoc);
            break;
        case '|':
            if (match('|')) return makeToken(TokenKind::OrOr,   start, startLoc);
            break;
    }

    diag_.error(startLoc, std::string("unexpected char '") + c + "'");
    return makeToken(TokenKind::Invalid, start, startLoc);
}

Token Lexer::lexIdentifierOrKeyword(size_t start, SourceLoc startLoc) {
    while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_') {
        advance();
    }
    std::string_view lex = src_.substr(start, pos_ - start);

    // таблица ключевых слов — compile-time порядок не важен, поиск линейный
    static constexpr std::pair<std::string_view, TokenKind> kws[] = {
        {"contract",  TokenKind::KwContract},
        {"function",  TokenKind::KwFunction},
        {"public",    TokenKind::KwPublic},
        {"returns",   TokenKind::KwReturns},
        {"return",    TokenKind::KwReturn},
        {"if",        TokenKind::KwIf},
        {"else",      TokenKind::KwElse},
        {"require",   TokenKind::KwRequire},
        {"mapping",   TokenKind::KwMapping},
        {"throw",     TokenKind::KwThrow},
        {"pragma",    TokenKind::KwPragma},
        {"modifier",  TokenKind::KwModifier},
        // uint — распространённый алиас uint256 в боевом Solidity
        {"uint",      TokenKind::KwUint256},
        {"true",      TokenKind::True},
        {"false",     TokenKind::False},
        {"uint256",   TokenKind::KwUint256},
        {"bool",      TokenKind::KwBool},
        {"address",   TokenKind::KwAddress},
    };
    for (auto& [name, k] : kws) {
        if (name == lex) return Token{k, lex, startLoc};
    }
    return Token{TokenKind::Identifier, lex, startLoc};
}

Token Lexer::lexString(size_t start, SourceLoc startLoc) {
    // на входе открывающая '"' уже потреблена. читаем до закрывающей '"'.
    // в MVP экранирование не поддерживаем — для low-level call достаточно ""
    while (peek() != '"' && peek() != '\0' && peek() != '\n') advance();
    if (peek() != '"') {
        diag_.error(startLoc, "unterminated string literal");
        return makeToken(TokenKind::Invalid, start, startLoc);
    }
    advance();  // закрывающая '"'
    return makeToken(TokenKind::StringLit, start, startLoc);
}

Token Lexer::lexNumber(size_t start, SourceLoc startLoc) {
    // hex-литерал 0x... (адреса, битовые константы) — первая цифра '0' уже потреблена
    if (src_[start] == '0' && (peek() == 'x' || peek() == 'X')) {
        advance();  // 'x'
        size_t hexStart = pos_;
        while (std::isxdigit(static_cast<unsigned char>(peek()))) advance();
        if (pos_ == hexStart) {
            diag_.error(startLoc, "hex literal has no digits");
        }
        return Token{TokenKind::Number, src_.substr(start, pos_ - start), startLoc};
    }
    while (std::isdigit(static_cast<unsigned char>(peek()))) advance();
    std::string_view lex = src_.substr(start, pos_ - start);
    // '0' одиночный ок, но '007' — нет (Solidity 0.5+); для hex проверка не применяется
    if (lex.size() > 1 && lex[0] == '0') {
        diag_.error(startLoc, "number literal with leading zero is not allowed");
    }
    return Token{TokenKind::Number, lex, startLoc};
}

}  // namespace solc
