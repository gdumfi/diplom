#include <gtest/gtest.h>
#include <vector>
#include <fstream>
#include <sstream>
#include "solc/diag/Diagnostic.h"
#include "solc/lexer/Lexer.h"

static std::string slurpFixture(const std::string& relPath) {
    std::ifstream f(std::string(SOLC_TESTS_DIR) + "/" + relPath);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::vector<solc::TokenKind> kinds(std::string_view src) {
    solc::DiagnosticEngine d;
    solc::Lexer lex(src, d);
    std::vector<solc::TokenKind> ks;
    for (auto t = lex.next();; t = lex.next()) {
        ks.push_back(t.kind);
        if (t.kind == solc::TokenKind::EndOfFile) break;
    }
    return ks;
}

TEST(Lexer, SingleCharPunct) {
    auto ks = kinds("(){},;+-*/.");
    using K = solc::TokenKind;
    std::vector<K> expected = {
        K::LParen, K::RParen, K::LBrace, K::RBrace,
        K::Comma, K::Semicolon,
        K::Plus, K::Minus, K::Star, K::Slash, K::Dot,
        K::EndOfFile};
    EXPECT_EQ(ks, expected);
}

TEST(Lexer, MultiCharOps) {
    auto ks = kinds("== != <= >= < > && || ! =");
    using K = solc::TokenKind;
    std::vector<K> expected = {
        K::EqEq, K::NotEq, K::LtEq, K::GtEq, K::Lt, K::Gt,
        K::AndAnd, K::OrOr, K::Not, K::Eq, K::EndOfFile};
    EXPECT_EQ(ks, expected);
}

TEST(Lexer, KeywordsAndIdents) {
    auto ks = kinds("contract function public returns return if else require "
                    "true false uint256 bool address foo bar_42");
    using K = solc::TokenKind;
    std::vector<K> expected = {
        K::KwContract, K::KwFunction, K::KwPublic, K::KwReturns, K::KwReturn,
        K::KwIf, K::KwElse, K::KwRequire,
        K::True, K::False,
        K::KwUint256, K::KwBool, K::KwAddress,
        K::Identifier, K::Identifier, K::EndOfFile};
    EXPECT_EQ(ks, expected);
}

TEST(Lexer, Numbers) {
    auto ks = kinds("0 1 42 1000000000000000000000000000");
    using K = solc::TokenKind;
    EXPECT_EQ(ks, (std::vector<K>{K::Number, K::Number, K::Number,
                                  K::Number, K::EndOfFile}));
}

TEST(Lexer, NumberLexemePreserved) {
    solc::DiagnosticEngine d;
    solc::Lexer lx("42", d);
    auto t = lx.next();
    EXPECT_EQ(t.kind, solc::TokenKind::Number);
    EXPECT_EQ(t.lexeme, "42");
}

TEST(Diag, CollectsErrors) {
    solc::DiagnosticEngine d;
    d.error({1, 5}, "bad token");
    EXPECT_EQ(d.errors().size(), 1u);
    EXPECT_EQ(d.errors()[0].loc.line, 1u);
    EXPECT_EQ(d.errors()[0].loc.col, 5u);
    EXPECT_EQ(d.errors()[0].message, "bad token");
    EXPECT_TRUE(d.hasErrors());
}

TEST(Lexer, CounterFixtureNoDiagnostics) {
    auto src = slurpFixture("fixtures/counter.sol");
    ASSERT_FALSE(src.empty()) << "fixture not found or empty";

    solc::DiagnosticEngine d;
    solc::Lexer lx(src, d);
    size_t n = 0;
    while (lx.next().kind != solc::TokenKind::EndOfFile) n++;

    EXPECT_FALSE(d.hasErrors()) << "lexer reported diagnostics on Counter.sol";
    EXPECT_GT(n, 20u);
}

TEST(Lexer, CommentsFixtureLexesCleanly) {
    auto src = slurpFixture("fixtures/lex_comments.sol");
    ASSERT_FALSE(src.empty());

    solc::DiagnosticEngine d;
    solc::Lexer lx(src, d);
    while (lx.next().kind != solc::TokenKind::EndOfFile) {}
    EXPECT_FALSE(d.hasErrors());
}

TEST(Lexer, UnexpectedCharProducesDiag) {
    solc::DiagnosticEngine d;
    solc::Lexer lx("contract @", d);
    while (lx.next().kind != solc::TokenKind::EndOfFile) {}
    ASSERT_TRUE(d.hasErrors());
    EXPECT_NE(d.errors()[0].message.find("unexpected"), std::string::npos);
}

TEST(Lexer, UnterminatedBlockCommentProducesDiag) {
    solc::DiagnosticEngine d;
    solc::Lexer lx("contract X { /* never closed", d);
    while (lx.next().kind != solc::TokenKind::EndOfFile) {}
    ASSERT_TRUE(d.hasErrors());
    EXPECT_NE(d.errors()[0].message.find("unterminated"), std::string::npos);
}

TEST(Lexer, LeadingZeroProducesDiag) {
    solc::DiagnosticEngine d;
    solc::Lexer lx("007", d);
    auto t = lx.next();
    EXPECT_EQ(t.kind, solc::TokenKind::Number);   // токен всё ещё возвращаем
    EXPECT_TRUE(d.hasErrors());
    EXPECT_NE(d.errors()[0].message.find("leading zero"), std::string::npos);
}

TEST(Lexer, SingleZeroIsFine) {
    solc::DiagnosticEngine d;
    solc::Lexer lx("0", d);
    auto t = lx.next();
    EXPECT_EQ(t.kind, solc::TokenKind::Number);
    EXPECT_EQ(t.lexeme, "0");
    EXPECT_FALSE(d.hasErrors());
}

TEST(Lexer, ZeroFollowedBySpaceIsFine) {
    solc::DiagnosticEngine d;
    solc::Lexer lx("0 1", d);
    while (lx.next().kind != solc::TokenKind::EndOfFile) {}
    EXPECT_FALSE(d.hasErrors());
}

TEST(Lexer, ColonToken) {
    auto ks = kinds(":");
    using K = solc::TokenKind;
    std::vector<K> expected = {K::Colon, K::EndOfFile};
    EXPECT_EQ(ks, expected);
}

TEST(Lexer, EmptyStringLiteral) {
    solc::DiagnosticEngine d;
    solc::Lexer lx("\"\"", d);
    auto t = lx.next();
    EXPECT_EQ(t.kind, solc::TokenKind::StringLit);
    EXPECT_EQ(t.lexeme, "\"\"");
    EXPECT_FALSE(d.hasErrors());
}

TEST(Lexer, NonEmptyStringLiteral) {
    solc::DiagnosticEngine d;
    solc::Lexer lx("\"abc\"", d);
    auto t = lx.next();
    EXPECT_EQ(t.kind, solc::TokenKind::StringLit);
    EXPECT_EQ(t.lexeme, "\"abc\"");
    EXPECT_FALSE(d.hasErrors());
}

TEST(Lexer, UnterminatedStringIsError) {
    solc::DiagnosticEngine d;
    solc::Lexer lx("\"abc", d);
    auto t = lx.next();
    EXPECT_EQ(t.kind, solc::TokenKind::Invalid);
    EXPECT_TRUE(d.hasErrors());
}

TEST(Lexer, LowLevelCallTokenSequence) {
    auto ks = kinds("msg.sender.call{value: x}(\"\")");
    using K = solc::TokenKind;
    std::vector<K> expected = {
        K::Identifier, K::Dot, K::Identifier, K::Dot, K::Identifier,
        K::LBrace, K::Identifier, K::Colon, K::Identifier, K::RBrace,
        K::LParen, K::StringLit, K::RParen, K::EndOfFile};
    EXPECT_EQ(ks, expected);
}
