#pragma once
#include <string_view>
#include "solc/diag/Diagnostic.h"

namespace solc {

enum class TokenKind {
    // литералы и идентификаторы
    Identifier, Number, True, False, StringLit,

    // типы
    KwUint256, KwBool, KwAddress,

    // ключевые слова
    KwContract, KwFunction, KwPublic, KwReturns, KwReturn,
    KwIf, KwElse, KwRequire, KwMapping, KwThrow, KwPragma, KwModifier,

    // операторы
    Plus, Minus, Star, Slash, Percent, Caret,
    Eq, EqEq, NotEq, Lt, Gt, LtEq, GtEq,
    PlusEq, MinusEq,
    AndAnd, OrOr, Not, FatArrow,

    // пунктуация
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Comma, Semicolon, Dot, Colon, Question,

    // служебные
    EndOfFile, Invalid,
};

struct Token {
    TokenKind        kind = TokenKind::Invalid;
    std::string_view lexeme;
    SourceLoc        loc;
};

const char* tokenKindName(TokenKind k);

}  // namespace solc
