# Week 2 — Lexer

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans.

**Goal:** Написать ручной лексер (~300 строк), превращающий исходник `.sol` в поток `Token`. Зелёные unit-тесты минимум на 5 фикстурах.

**Architecture:** Однопроходный лексер, работающий с `std::string_view` (zero-copy). Lookahead 1 символ (`peek()`). Источник — вход lexera; `SourceLoc{line, col}` копируется в каждый токен. Комментарии (`//`, `/* */`) и whitespace пропускаются. Ошибка лексера = специальный `TokenKind::Invalid` с позицией; `DiagnosticEngine` собирает ошибки, но сам парсер/тесты могут инспектировать их через `Lexer::diagnostics()`.

**Tech Stack:** C++20, `std::string_view`, `std::variant` для literal payload (не обязательно — можно плоскую структуру).

---

## Files

- Create: `include/solc/diag/Diagnostic.h`
- Create: `src/diag/Diagnostic.cpp`
- Create: `include/solc/lexer/Token.h`
- Create: `include/solc/lexer/Lexer.h`
- Create: `src/lexer/Lexer.cpp`
- Modify: `src/CMakeLists.txt` — добавить новые `.cpp`
- Create: `tests/test_lexer.cpp`
- Modify: `tests/CMakeLists.txt` — добавить `test_lexer.cpp`
- Create: `tests/fixtures/lex_simple.sol`, `lex_counter.sol`, `lex_ops.sol`, `lex_comments.sol`, `lex_invalid.sol`

---

## Подмножество токенов (полный список)

**Keywords:** `contract`, `function`, `public`, `returns`, `return`, `if`, `else`, `require`, `true`, `false`, `uint256`, `bool`, `address`.

**Identifiers:** `[A-Za-z_][A-Za-z0-9_]*` (после исключения keywords).

**Number literals:** десятичные `[0-9]+` (hex, underscore, scientific — НЕ поддерживаем в MVP).

**Operators & punctuation:**
`+ - * /`, `== != < > <= >=`, `&& || !`, `=`, `(`, `)`, `{`, `}`, `,`, `;`, `.`.

**Служебные:** `EndOfFile`, `Invalid`.

Итого ≈ 35 `TokenKind`.

---

## Task 1: SourceLoc и DiagnosticEngine

- [ ] **Step 1.1: Failing test для Diagnostic**

Создать `tests/test_lexer.cpp`:

```cpp
#include <gtest/gtest.h>
#include "solc/diag/Diagnostic.h"

TEST(Diag, CollectsErrors) {
    solc::DiagnosticEngine d;
    d.error({1, 5}, "bad token");
    EXPECT_EQ(d.errors().size(), 1u);
    EXPECT_EQ(d.errors()[0].loc.line, 1);
    EXPECT_EQ(d.errors()[0].loc.col, 5);
    EXPECT_EQ(d.errors()[0].message, "bad token");
    EXPECT_TRUE(d.hasErrors());
}
```

- [ ] **Step 1.2: `include/solc/diag/Diagnostic.h`**

```cpp
#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace solc {

struct SourceLoc {
    uint32_t line = 1;
    uint32_t col  = 1;
};

struct Diagnostic {
    SourceLoc loc;
    std::string message;
};

class DiagnosticEngine {
public:
    void error(SourceLoc loc, std::string msg);
    const std::vector<Diagnostic>& errors() const { return errors_; }
    bool hasErrors() const { return !errors_.empty(); }
private:
    std::vector<Diagnostic> errors_;
};

}  // namespace solc
```

- [ ] **Step 1.3: `src/diag/Diagnostic.cpp`**

```cpp
#include "solc/diag/Diagnostic.h"

namespace solc {

void DiagnosticEngine::error(SourceLoc loc, std::string msg) {
    errors_.push_back({loc, std::move(msg)});
}

}  // namespace solc
```

- [ ] **Step 1.4: Подключить в сборку**

Обновить `src/CMakeLists.txt`:

```cmake
add_library(solc_core STATIC
    diag/Diagnostic.cpp
    driver/Driver.cpp
)
```

Обновить `tests/CMakeLists.txt`:

```cmake
add_executable(unit_tests
    test_smoke.cpp
    test_lexer.cpp
)
```

- [ ] **Step 1.5: Ninja + ctest**

```bash
cd build && ninja && ctest --output-on-failure
```

Expected: `Diag.CollectsErrors` PASS.

- [ ] **Step 1.6: Коммит**

```bash
git add include/solc/diag src/diag src/CMakeLists.txt tests/CMakeLists.txt tests/test_lexer.cpp
git commit -m "feat(diag): DiagnosticEngine with SourceLoc"
```

---

## Task 2: Token и TokenKind

- [ ] **Step 2.1: `include/solc/lexer/Token.h`**

```cpp
#pragma once
#include <string_view>
#include "solc/diag/Diagnostic.h"

namespace solc {

enum class TokenKind {
    // literals & ident
    Identifier, Number, True, False,

    // types
    KwUint256, KwBool, KwAddress,

    // keywords
    KwContract, KwFunction, KwPublic, KwReturns, KwReturn,
    KwIf, KwElse, KwRequire,

    // operators
    Plus, Minus, Star, Slash,
    Eq, EqEq, NotEq, Lt, Gt, LtEq, GtEq,
    AndAnd, OrOr, Not,

    // punctuation
    LParen, RParen, LBrace, RBrace, Comma, Semicolon, Dot,

    // control
    EndOfFile, Invalid,
};

struct Token {
    TokenKind        kind = TokenKind::Invalid;
    std::string_view lexeme;     // slice of source
    SourceLoc        loc;
};

const char* tokenKindName(TokenKind k);

}  // namespace solc
```

- [ ] **Step 2.2: `src/lexer/Lexer.cpp` — начало: `tokenKindName`**

Пока создать пустой `Lexer.cpp` с функцией:

```cpp
#include "solc/lexer/Token.h"

namespace solc {

const char* tokenKindName(TokenKind k) {
    switch (k) {
#define CASE(x) case TokenKind::x: return #x;
        CASE(Identifier) CASE(Number) CASE(True) CASE(False)
        CASE(KwUint256) CASE(KwBool) CASE(KwAddress)
        CASE(KwContract) CASE(KwFunction) CASE(KwPublic)
        CASE(KwReturns) CASE(KwReturn) CASE(KwIf) CASE(KwElse) CASE(KwRequire)
        CASE(Plus) CASE(Minus) CASE(Star) CASE(Slash)
        CASE(Eq) CASE(EqEq) CASE(NotEq) CASE(Lt) CASE(Gt) CASE(LtEq) CASE(GtEq)
        CASE(AndAnd) CASE(OrOr) CASE(Not)
        CASE(LParen) CASE(RParen) CASE(LBrace) CASE(RBrace)
        CASE(Comma) CASE(Semicolon) CASE(Dot)
        CASE(EndOfFile) CASE(Invalid)
#undef CASE
    }
    return "?";
}

}  // namespace solc
```

Добавить `lexer/Lexer.cpp` в `src/CMakeLists.txt`.

- [ ] **Step 2.3: Смоук-тест — компилируется ли**

```bash
cd build && ninja unit_tests
```

Expected: компиляция проходит.

- [ ] **Step 2.4: Коммит**

```bash
git add include/solc/lexer/Token.h src/lexer/Lexer.cpp src/CMakeLists.txt
git commit -m "feat(lexer): Token, TokenKind and tokenKindName()"
```

---

## Task 3: Lexer API + токенизация однобайтовых пунктуаторов

- [ ] **Step 3.1: Failing test — single chars**

Дописать в `tests/test_lexer.cpp`:

```cpp
#include "solc/lexer/Lexer.h"

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
```

- [ ] **Step 3.2: `include/solc/lexer/Lexer.h`**

```cpp
#pragma once
#include <string_view>
#include "solc/lexer/Token.h"
#include "solc/diag/Diagnostic.h"

namespace solc {

class Lexer {
public:
    Lexer(std::string_view source, DiagnosticEngine& diag)
        : src_(source), diag_(diag) {}

    Token next();                       // returns EndOfFile at end
private:
    char peek(size_t offset = 0) const;
    char advance();
    bool match(char c);
    void skipWhitespaceAndComments();

    Token makeToken(TokenKind k, size_t start) const;
    Token lexIdentifierOrKeyword(size_t start);
    Token lexNumber(size_t start);

    std::string_view src_;
    DiagnosticEngine& diag_;
    size_t    pos_  = 0;
    SourceLoc loc_{1, 1};
};

}  // namespace solc
```

- [ ] **Step 3.3: `src/lexer/Lexer.cpp` — skeleton + single-char punct**

```cpp
#include "solc/lexer/Lexer.h"
#include <cctype>

namespace solc {

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
            advance(); advance();
            while (peek() != '\0' && !(peek() == '*' && peek(1) == '/')) advance();
            if (peek() == '*') { advance(); advance(); }
        } else {
            return;
        }
    }
}

Token Lexer::makeToken(TokenKind k, size_t start) const {
    return Token{k, src_.substr(start, pos_ - start), loc_};
}

Token Lexer::next() {
    skipWhitespaceAndComments();
    if (pos_ >= src_.size()) return {TokenKind::EndOfFile, {}, loc_};

    SourceLoc startLoc = loc_;
    size_t    start    = pos_;
    char      c        = advance();

    auto one = [&](TokenKind k) {
        return Token{k, src_.substr(start, 1), startLoc};
    };

    switch (c) {
        case '(': return one(TokenKind::LParen);
        case ')': return one(TokenKind::RParen);
        case '{': return one(TokenKind::LBrace);
        case '}': return one(TokenKind::RBrace);
        case ',': return one(TokenKind::Comma);
        case ';': return one(TokenKind::Semicolon);
        case '.': return one(TokenKind::Dot);
        case '+': return one(TokenKind::Plus);
        case '-': return one(TokenKind::Minus);
        case '*': return one(TokenKind::Star);
        case '/': return one(TokenKind::Slash);
    }

    diag_.error(startLoc, std::string("unexpected char '") + c + "'");
    return Token{TokenKind::Invalid, src_.substr(start, 1), startLoc};
}

}  // namespace solc
```

- [ ] **Step 3.4: Зелёный SingleCharPunct**

```bash
cd build && ninja && ctest -R SingleCharPunct --output-on-failure
```

Expected: PASS.

- [ ] **Step 3.5: Коммит**

```bash
git add include/solc/lexer/Lexer.h src/lexer/Lexer.cpp tests/test_lexer.cpp
git commit -m "feat(lexer): single-char punctuation + whitespace/comments"
```

---

## Task 4: Multi-char operators (`==`, `!=`, `<=`, `>=`, `&&`, `||`, `!`, `=`, `<`, `>`)

- [ ] **Step 4.1: Failing test**

```cpp
TEST(Lexer, MultiCharOps) {
    auto ks = kinds("== != <= >= < > && || ! =");
    using K = solc::TokenKind;
    std::vector<K> expected = {
        K::EqEq, K::NotEq, K::LtEq, K::GtEq, K::Lt, K::Gt,
        K::AndAnd, K::OrOr, K::Not, K::Eq, K::EndOfFile};
    EXPECT_EQ(ks, expected);
}
```

- [ ] **Step 4.2: Расширить `Lexer::next()` — вместо старого switch**

Заменить блок `switch` в `Lexer.cpp` на:

```cpp
    switch (c) {
        case '(': return one(TokenKind::LParen);
        case ')': return one(TokenKind::RParen);
        case '{': return one(TokenKind::LBrace);
        case '}': return one(TokenKind::RBrace);
        case ',': return one(TokenKind::Comma);
        case ';': return one(TokenKind::Semicolon);
        case '.': return one(TokenKind::Dot);
        case '+': return one(TokenKind::Plus);
        case '-': return one(TokenKind::Minus);
        case '*': return one(TokenKind::Star);
        case '/': return one(TokenKind::Slash);

        case '=':
            if (match('=')) return makeToken(TokenKind::EqEq, start);
            return one(TokenKind::Eq);
        case '!':
            if (match('=')) return makeToken(TokenKind::NotEq, start);
            return one(TokenKind::Not);
        case '<':
            if (match('=')) return makeToken(TokenKind::LtEq, start);
            return one(TokenKind::Lt);
        case '>':
            if (match('=')) return makeToken(TokenKind::GtEq, start);
            return one(TokenKind::Gt);
        case '&':
            if (match('&')) return makeToken(TokenKind::AndAnd, start);
            break;
        case '|':
            if (match('|')) return makeToken(TokenKind::OrOr, start);
            break;
    }
```

- [ ] **Step 4.3: Зелёный**

```bash
cd build && ninja && ctest -R MultiCharOps --output-on-failure
```

- [ ] **Step 4.4: Коммит**

```bash
git commit -am "feat(lexer): multi-char operators"
```

---

## Task 5: Идентификаторы и ключевые слова

- [ ] **Step 5.1: Failing test**

```cpp
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
```

- [ ] **Step 5.2: Добавить в `next()` ветку ident, и реализацию `lexIdentifierOrKeyword`**

В `next()` перед `switch`:

```cpp
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
        return lexIdentifierOrKeyword(start);
    }
```

(Обрати внимание: `c` уже advance'нут. `lexIdentifierOrKeyword` дочитает хвост.)

Реализация:

```cpp
Token Lexer::lexIdentifierOrKeyword(size_t start) {
    while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_') advance();
    std::string_view lex = src_.substr(start, pos_ - start);

    static const std::pair<std::string_view, TokenKind> kws[] = {
        {"contract",  TokenKind::KwContract},
        {"function",  TokenKind::KwFunction},
        {"public",    TokenKind::KwPublic},
        {"returns",   TokenKind::KwReturns},
        {"return",    TokenKind::KwReturn},
        {"if",        TokenKind::KwIf},
        {"else",      TokenKind::KwElse},
        {"require",   TokenKind::KwRequire},
        {"true",      TokenKind::True},
        {"false",     TokenKind::False},
        {"uint256",   TokenKind::KwUint256},
        {"bool",      TokenKind::KwBool},
        {"address",   TokenKind::KwAddress},
    };
    for (auto& [name, k] : kws) if (name == lex)
        return {k, lex, {}};
    return {TokenKind::Identifier, lex, {}};
}
```

(SourceLoc для token прописывать в `next()` после вызова: `tok.loc = startLoc;`. Добавь это или вынеси start-loc захват в helper.)

- [ ] **Step 5.3: Зелёный**

```bash
ninja && ctest -R KeywordsAndIdents --output-on-failure
```

- [ ] **Step 5.4: Коммит**

```bash
git commit -am "feat(lexer): identifiers and keywords"
```

---

## Task 6: Числовые литералы

- [ ] **Step 6.1: Failing test**

```cpp
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
```

- [ ] **Step 6.2: Ветка в `next()`**

```cpp
    if (std::isdigit(static_cast<unsigned char>(c))) {
        return lexNumber(start);
    }

// ...

Token Lexer::lexNumber(size_t start) {
    while (std::isdigit(static_cast<unsigned char>(peek()))) advance();
    return makeToken(TokenKind::Number, start);
}
```

- [ ] **Step 6.3: Зелёный + коммит**

```bash
ninja && ctest --output-on-failure
git commit -am "feat(lexer): decimal number literals"
```

---

## Task 7: Фикстуры и интеграционный тест на Counter.sol

- [ ] **Step 7.1: Создать фикстуры**

`tests/fixtures/lex_counter.sol`:

```solidity
contract Counter {
    uint256 public value;
    address public owner;

    function increment(uint256 delta) public returns (uint256) {
        require(msg.sender == owner);
        uint256 newValue = value + delta;
        value = newValue;
        return newValue;
    }
}
```

`tests/fixtures/lex_comments.sol`:

```solidity
// top comment
contract X { /* inline */ uint256 x; }
```

- [ ] **Step 7.2: Тест: лексер не выдаёт ошибок на Counter**

```cpp
#include <fstream>
#include <sstream>

static std::string slurp(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

TEST(Lexer, CounterFixtureNoDiagnostics) {
    auto src = slurp("tests/fixtures/lex_counter.sol");
    ASSERT_FALSE(src.empty());
    solc::DiagnosticEngine d;
    solc::Lexer lx(src, d);
    size_t n = 0;
    while (lx.next().kind != solc::TokenKind::EndOfFile) n++;
    EXPECT_FALSE(d.hasErrors());
    EXPECT_GT(n, 20u);
}
```

- [ ] **Step 7.3: Прокинуть path к фикстурам**

В `tests/CMakeLists.txt` добавить:

```cmake
add_custom_target(copy_fixtures ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures
            ${CMAKE_CURRENT_BINARY_DIR}/tests/fixtures
)
set_tests_properties(Lexer.CounterFixtureNoDiagnostics
    PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/tests)
```

(Или проще: в тесте использовать `TEST_SRC_DIR`, передав его через `target_compile_definitions`.)

- [ ] **Step 7.4: Зелёный + коммит**

```bash
ninja && ctest --output-on-failure
git add tests/fixtures tests/test_lexer.cpp tests/CMakeLists.txt
git commit -m "test(lexer): Counter.sol fixture lexed without diagnostics"
```

---

## Task 8: Негативный тест (Invalid token)

- [ ] **Step 8.1: Test**

```cpp
TEST(Lexer, UnexpectedChar) {
    solc::DiagnosticEngine d;
    solc::Lexer lx("contract @", d);
    while (lx.next().kind != solc::TokenKind::EndOfFile) {}
    ASSERT_TRUE(d.hasErrors());
    EXPECT_NE(d.errors()[0].message.find("unexpected"), std::string::npos);
}
```

- [ ] **Step 8.2: Зелёный + коммит**

```bash
ninja && ctest --output-on-failure
git commit -am "test(lexer): invalid char produces diagnostic"
```

---

## Критерий готовности недели 2

- [ ] ≥ 6 тестовых кейсов в `test_lexer.cpp`, все зелёные
- [ ] `Counter.sol`-фикстура лексируется без ошибок
- [ ] Покрыты все 35 `TokenKind`
- [ ] Позиция (`SourceLoc`) сохраняется в `Token.loc`
- [ ] Комментарии `//` и `/* */` корректно пропускаются
- [ ] Минимум 6 коммитов (по одному на task)
