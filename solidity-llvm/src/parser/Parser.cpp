#include "solc/parser/Parser.h"

#include <string>

namespace solc {

namespace {
// RAII-счётчик глубины рекурсии
struct DepthGuard {
    int& d;
    explicit DepthGuard(int& x) : d(x) { ++d; }
    ~DepthGuard() { --d; }
};
}  // namespace

Parser::Parser(Lexer& lex, DiagnosticEngine& diag)
    : lex_(lex), diag_(diag), current_(lex.next()) {}

Token Parser::advance() {
    Token t = current_;
    current_ = lex_.next();
    return t;
}

bool Parser::match(TokenKind k) {
    if (!check(k)) return false;
    advance();
    return true;
}

bool Parser::expect(TokenKind k, std::string_view ctx) {
    if (check(k)) { advance(); return true; }
    std::string msg = "expected ";
    msg.append(tokenKindName(k));
    msg += " in ";
    msg.append(ctx);
    msg += ", got ";
    msg.append(tokenKindName(current_.kind));
    diag_.error(current_.loc, std::move(msg));
    return false;
}

void Parser::synchronizeToSemicolon() {
    while (!check(TokenKind::EndOfFile) &&
           !check(TokenKind::RBrace) &&
           !check(TokenKind::Semicolon)) {
        advance();
    }
    if (check(TokenKind::Semicolon)) advance();
}

void Parser::synchronizeToBraceEnd() {
    while (!check(TokenKind::EndOfFile) && !check(TokenKind::LBrace) &&
           !check(TokenKind::RBrace)) {
        advance();
    }
    if (check(TokenKind::LBrace)) skipBraceBody();
}

std::optional<ast::TypeKind> Parser::parseSimpleTypeKind() {
    switch (peek().kind) {
        case TokenKind::KwUint256: advance(); return ast::TypeKind::Uint256;
        case TokenKind::KwBool:    advance(); return ast::TypeKind::Bool;
        case TokenKind::KwAddress: advance(); return ast::TypeKind::Address;
        default:
            // user-тип (struct/контракт) в ключе/значении mapping — моделируем как address
            if (check(TokenKind::Identifier)) { advance(); return ast::TypeKind::Address; }
            diag_.error(peek().loc, std::string("expected simple type (uint256/bool/address), got ")
                                        + tokenKindName(peek().kind));
            return std::nullopt;
    }
}

std::optional<ast::Type> Parser::parseType() {
    SourceLoc loc = peek().loc;
    // mapping — особый случай (ключ/значение), возвращаем сразу без array-суффикса
    if (check(TokenKind::KwMapping)) {
        advance();
        if (!expect(TokenKind::LParen, "mapping type")) return std::nullopt;
        auto key = parseSimpleTypeKind();
        if (!key) return std::nullopt;
        if (!expect(TokenKind::FatArrow, "mapping type")) return std::nullopt;
        auto val = parseSimpleTypeKind();
        if (!val) return std::nullopt;
        if (!expect(TokenKind::RParen, "mapping type")) return std::nullopt;
        ast::Type t;
        t.kind = ast::TypeKind::Mapping; t.loc = loc; t.mapKey = *key; t.mapValue = *val;
        return t;
    }
    std::optional<ast::Type> base;
    switch (peek().kind) {
        case TokenKind::KwUint256: advance(); base = ast::Type{ast::TypeKind::Uint256, loc}; break;
        case TokenKind::KwBool:    advance(); base = ast::Type{ast::TypeKind::Bool,    loc}; break;
        case TokenKind::KwAddress: advance(); base = ast::Type{ast::TypeKind::Address, loc}; break;
        default:
            // пользовательский тип (контракт/интерфейс/struct, напр. ERC20, Message) —
            // на уровне значения это адрес/opaque; моделируем как address
            if (check(TokenKind::Identifier)) { advance(); base = ast::Type{ast::TypeKind::Address, loc}; }
            break;
    }
    if (!base) {
        diag_.error(loc, std::string("expected type (uint256/bool/address/mapping/user-type), got ")
                             + tokenKindName(peek().kind));
        return std::nullopt;
    }
    // суффиксы массива T[] / T[N] — семантику массива не моделируем, тип opaque (address)
    while (check(TokenKind::LBracket)) {
        advance();
        while (!check(TokenKind::RBracket) && !check(TokenKind::EndOfFile)) advance();
        if (!expect(TokenKind::RBracket, "array type")) return std::nullopt;
        base = ast::Type{ast::TypeKind::Address, loc};
    }
    return base;
}

std::optional<ast::StateVarDecl> Parser::parseStateVarDecl() {
    SourceLoc startLoc = peek().loc;

    auto ty = parseType();
    if (!ty) { synchronizeToSemicolon(); return std::nullopt; }

    // атрибуты между типом и именем: visibility (public/private/internal),
    // mutability (constant/immutable) и т.п. — в боевом Solidity в любом порядке.
    // имя переменной — ПОСЛЕДНИЙ идентификатор перед '=' или ';'; предыдущие — атрибуты
    bool isPublic = false;
    std::string name;
    bool haveName = false;
    for (;;) {
        if (check(TokenKind::KwPublic)) { isPublic = true; advance(); continue; }
        if (check(TokenKind::Identifier)) {
            name = std::string(peek().lexeme);
            advance();
            if (check(TokenKind::Eq) || check(TokenKind::Semicolon)) { haveName = true; break; }
            if (std::string_view(peek().lexeme) == "public") isPublic = true;
            // это был атрибут, не имя — продолжаем искать имя
            continue;
        }
        break;
    }
    if (!haveName) {
        diag_.error(peek().loc, "expected identifier in state variable declaration");
        synchronizeToSemicolon();
        return std::nullopt;
    }

    // необязательный инициализатор: uint x = expr;  — в EVM он исполняется в
    // конструкторе; для анализа безопасности значение не важно, разбираем и отбрасываем
    if (match(TokenKind::Eq)) {
        auto init = parseExpr();
        if (!init) { synchronizeToSemicolon(); return std::nullopt; }
    }

    if (!expect(TokenKind::Semicolon, "state variable declaration")) {
        synchronizeToSemicolon();
        return std::nullopt;
    }

    return ast::StateVarDecl{*ty, std::move(name), isPublic, -1, startLoc};
}

std::optional<ast::Param> Parser::parseParam() {
    SourceLoc loc = peek().loc;
    auto ty = parseType();
    if (!ty) return std::nullopt;
    if (!check(TokenKind::Identifier)) {
        diag_.error(peek().loc, "expected parameter name");
        return std::nullopt;
    }
    std::string name(peek().lexeme);
    advance();
    return ast::Param{*ty, std::move(name), loc};
}

void Parser::skipPragma() {
    // pragma solidity ^0.4.24;  — версия анализатору не важна, пропускаем до ';'
    advance();  // 'pragma'
    while (!check(TokenKind::Semicolon) && !check(TokenKind::EndOfFile)) advance();
    if (check(TokenKind::Semicolon)) advance();
}

bool Parser::skipParenGroup() {
    // на входе current_ должен быть '('. потребляем до парной ')' включительно.
    // используется для игнорируемых аргументов данных low-level call (напр. "")
    if (!check(TokenKind::LParen)) return false;
    int depth = 0;
    for (;;) {
        if (check(TokenKind::EndOfFile)) {
            diag_.error(peek().loc, "unterminated '(' group");
            return false;
        }
        if (check(TokenKind::LParen)) depth++;
        else if (check(TokenKind::RParen)) {
            depth--;
            if (depth == 0) { advance(); return true; }
        }
        advance();
    }
}

void Parser::skipBraceBody() {
    // на входе current_ должен быть '{'
    if (!check(TokenKind::LBrace)) return;
    SourceLoc openLoc = peek().loc;
    int depth = 0;
    for (;;) {
        if (check(TokenKind::EndOfFile)) {
            diag_.error(openLoc, "unterminated '{' body");
            return;
        }
        if (check(TokenKind::LBrace))      depth++;
        else if (check(TokenKind::RBrace)) {
            depth--;
            if (depth == 0) { advance(); return; }
        }
        advance();
    }
}

std::optional<ast::FunctionDecl> Parser::parseFunctionDecl() {
    SourceLoc startLoc = peek().loc;
    // вызывающий уже проверил KwFunction, поэтому здесь sync не нужен
    if (!expect(TokenKind::KwFunction, "function declaration")) return std::nullopt;

    // имя функции; безымянная function() { } — fallback (старый Solidity)
    std::string name;
    if (check(TokenKind::Identifier)) {
        name = std::string(peek().lexeme);
        advance();
    } else if (check(TokenKind::LParen)) {
        name = "$fallback";
    } else {
        diag_.error(peek().loc, "expected function name");
        synchronizeToBraceEnd();
        return std::nullopt;
    }

    if (!expect(TokenKind::LParen, "function parameter list")) {
        synchronizeToBraceEnd();
        return std::nullopt;
    }

    std::vector<ast::Param> params;
    if (!check(TokenKind::RParen)) {
        for (;;) {
            auto pr = parseParam();
            if (!pr) { synchronizeToBraceEnd(); return std::nullopt; }
            params.push_back(std::move(*pr));
            if (!match(TokenKind::Comma)) break;
        }
    }
    if (!expect(TokenKind::RParen, "function parameter list")) {
        synchronizeToBraceEnd();
        return std::nullopt;
    }

    // гибкая последовательность атрибутов до тела '{': visibility (public/...),
    // mutability (payable/view/pure/constant), кастомные модификаторы (onlyOwner[(...)]),
    // и returns(...) — в любом порядке. в боевом Solidity многое опционально;
    // в MVP visibility/mutability/модификаторы просто пропускаем (не моделируем)
    std::optional<ast::Type> retType;
    std::vector<std::string> modifierNames;   // применённые модификаторы (и mutability-слова)
    for (;;) {
        if (check(TokenKind::LBrace)) break;
        // безтеловое объявление (прототип интерфейса/abstract-функции) заканчивается ';'
        if (check(TokenKind::Semicolon)) break;

        if (check(TokenKind::KwReturns)) {
            advance();
            if (!expect(TokenKind::LParen, "returns clause")) {
                synchronizeToBraceEnd();
                return std::nullopt;
            }
            auto rt = parseType();
            if (!rt) { synchronizeToBraceEnd(); return std::nullopt; }
            retType = *rt;
            // несколько возвращаемых значений в MVP не поддержаны — берём первое,
            // остальные пропускаем до ')'
            while (!check(TokenKind::RParen) && !check(TokenKind::EndOfFile)) advance();
            if (!expect(TokenKind::RParen, "returns clause")) {
                synchronizeToBraceEnd();
                return std::nullopt;
            }
            continue;
        }

        // visibility (KwPublic) / mutability / кастомный модификатор — идентификатор
        if (check(TokenKind::KwPublic) || check(TokenKind::Identifier)) {
            // имя-идентификатор запоминаем как кандидат-модификатор (mutability-слова
            // тоже попадут, но codegen инлайнит только совпавшие с определением)
            if (check(TokenKind::Identifier)) modifierNames.emplace_back(peek().lexeme);
            advance();
            // модификатор с аргументами: onlyOwner(x) — пропускаем сбалансированные скобки
            if (check(TokenKind::LParen)) {
                int depth = 0;
                do {
                    if (check(TokenKind::LParen))      depth++;
                    else if (check(TokenKind::RParen)) depth--;
                    advance();
                } while (depth > 0 && !check(TokenKind::EndOfFile));
            }
            continue;
        }

        diag_.error(peek().loc, std::string("unexpected token in function header: ")
                                    + tokenKindName(peek().kind));
        synchronizeToBraceEnd();
        return std::nullopt;
    }

    // прототип без тела: function f(...) [returns(...)] ;  — интерфейс/abstract.
    // тело пустое; для анализа безопасности такой контракт — no-op (находок нет)
    if (match(TokenKind::Semicolon)) {
        ast::FunctionDecl fn;
        fn.name       = std::move(name);
        fn.params     = std::move(params);
        fn.returnType = retType;
        fn.modifiers  = std::move(modifierNames);
        fn.body.loc   = startLoc;
        fn.loc        = startLoc;
        return fn;
    }

    if (!check(TokenKind::LBrace)) {
        diag_.error(peek().loc, "expected '{' for function body");
        synchronizeToBraceEnd();
        return std::nullopt;
    }
    ast::Block body;
    body.loc = peek().loc;
    if (!parseBlockStatements(body.stmts)) {
        synchronizeToBraceEnd();
        return std::nullopt;
    }

    ast::FunctionDecl fn;
    fn.name       = std::move(name);
    fn.params     = std::move(params);
    fn.returnType = retType;
    fn.modifiers  = std::move(modifierNames);
    fn.body       = std::move(body);
    fn.loc        = startLoc;
    return fn;
}

std::optional<ast::ModifierDecl> Parser::parseModifierDecl() {
    SourceLoc startLoc = peek().loc;
    if (!expect(TokenKind::KwModifier, "modifier declaration")) return std::nullopt;

    if (!check(TokenKind::Identifier)) {
        diag_.error(peek().loc, "expected modifier name");
        synchronizeToBraceEnd();
        return std::nullopt;
    }
    std::string name(peek().lexeme);
    advance();

    // список параметров необязателен: modifier m { } или modifier m(uint x) { }
    std::vector<ast::Param> params;
    if (match(TokenKind::LParen)) {
        if (!check(TokenKind::RParen)) {
            for (;;) {
                auto pr = parseParam();
                if (!pr) { synchronizeToBraceEnd(); return std::nullopt; }
                params.push_back(std::move(*pr));
                if (!match(TokenKind::Comma)) break;
            }
        }
        if (!expect(TokenKind::RParen, "modifier parameter list")) {
            synchronizeToBraceEnd();
            return std::nullopt;
        }
    }

    if (!check(TokenKind::LBrace)) {
        diag_.error(peek().loc, "expected '{' for modifier body");
        synchronizeToBraceEnd();
        return std::nullopt;
    }
    ast::Block body;
    body.loc = peek().loc;
    if (!parseBlockStatements(body.stmts)) {
        synchronizeToBraceEnd();
        return std::nullopt;
    }

    ast::ModifierDecl md;
    md.name   = std::move(name);
    md.params = std::move(params);
    md.body   = std::move(body);
    md.loc    = startLoc;
    return md;
}

std::vector<ast::ContractDecl> Parser::parseSourceUnit() {
    std::vector<ast::ContractDecl> contracts;
    for (;;) {
        while (check(TokenKind::KwPragma)) skipPragma();
        // import ...; — пропускаем
        if (check(TokenKind::Identifier) && peek().lexeme == "import") {
            synchronizeToSemicolon();
            continue;
        }
        if (check(TokenKind::EndOfFile)) break;

        if (check(TokenKind::KwContract)) {
            size_t before = 0;  // защита от зацикливания на неконсумирующей ошибке
            (void)before;
            auto c = parseContract();
            if (c) contracts.push_back(std::move(*c));
            else break;   // ошибка разбора контракта — прекращаем
        } else if (check(TokenKind::Identifier) &&
                   (peek().lexeme == "interface" || peek().lexeme == "library" ||
                    peek().lexeme == "abstract")) {
            // интерфейсы/библиотеки/abstract не анализируем — пропускаем тело
            while (!check(TokenKind::LBrace) && !check(TokenKind::EndOfFile)) advance();
            if (check(TokenKind::LBrace)) skipBraceBody();
        } else {
            // неизвестный токен верхнего уровня — пропускаем, чтобы не зациклиться
            advance();
        }
    }
    return contracts;
}

std::optional<ast::ContractDecl> Parser::parseContract() {
    // пропускаем директивы pragma перед контрактом (боевой Solidity)
    while (check(TokenKind::KwPragma)) skipPragma();

    SourceLoc startLoc = peek().loc;
    if (!check(TokenKind::KwContract)) {
        diag_.error(peek().loc, "expected 'contract' at top level");
        return std::nullopt;
    }
    advance();

    if (!check(TokenKind::Identifier)) {
        diag_.error(peek().loc, "expected contract name");
        return std::nullopt;
    }
    std::string name(peek().lexeme);
    advance();

    // наследование: contract X is Base1, Base2(args) { ... } — сохраняем имена баз;
    // слияние членов делает пост-проход sema::mergeInheritance (аргументы Base(args)
    // для анализа безопасности не важны — пропускаем)
    std::vector<std::string> bases;
    if (check(TokenKind::Identifier) && peek().lexeme == "is") {
        advance();
        for (;;) {
            if (!check(TokenKind::Identifier)) break;
            bases.emplace_back(peek().lexeme);      // имя базы
            advance();
            if (check(TokenKind::LParen)) skipParenGroup();   // Base(args)
            if (!match(TokenKind::Comma)) break;
        }
    }

    if (!expect(TokenKind::LBrace, "contract body")) return std::nullopt;

    ast::ContractDecl c;
    c.name = std::move(name);
    c.baseContracts = std::move(bases);
    c.loc  = startLoc;

    while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
        // решение по первому токену: function / modifier / event|using / struct|enum / state-var.
        // event/using/struct/enum для анализа безопасности не моделируем — пропускаем
        // их объявления (event/using до ';', struct/enum — тело { })
        if (check(TokenKind::KwFunction)) {
            auto f = parseFunctionDecl();
            if (f) c.functions.push_back(std::move(*f));
            // на nullopt парсер уже синхронизировался — продолжаем
        } else if (check(TokenKind::KwModifier)) {
            auto md = parseModifierDecl();
            if (md) c.modifiers.push_back(std::move(*md));
        } else if (check(TokenKind::Identifier) &&
                   (peek().lexeme == "event" || peek().lexeme == "using")) {
            synchronizeToSemicolon();
        } else if (check(TokenKind::Identifier) &&
                   (peek().lexeme == "struct" || peek().lexeme == "enum")) {
            while (!check(TokenKind::LBrace) && !check(TokenKind::EndOfFile) &&
                   !check(TokenKind::RBrace)) advance();
            if (check(TokenKind::LBrace)) skipBraceBody();
        } else {
            auto sv = parseStateVarDecl();
            if (sv) c.stateVars.push_back(std::move(*sv));
        }
    }

    if (!expect(TokenKind::RBrace, "contract body")) return std::nullopt;
    return c;
}

ast::ExprPtr Parser::parsePrimary() {
    DepthGuard g(depth_);
    if (depth_ > kMaxDepth) {
        diag_.error(peek().loc, "expression nesting too deep");
        return nullptr;
    }
    SourceLoc loc = peek().loc;
    switch (peek().kind) {
        case TokenKind::Number: {
            std::string text(peek().lexeme);
            advance();
            // единица после числа (1 ether / 5 days / 100 wei) — для анализа
            // безопасности множитель не важен, суффикс поглощаем
            if (check(TokenKind::Identifier)) {
                std::string_view u = peek().lexeme;
                if (u == "wei" || u == "gwei" || u == "szabo" || u == "finney" ||
                    u == "ether" || u == "seconds" || u == "minutes" || u == "hours" ||
                    u == "days" || u == "weeks")
                    advance();
            }
            return std::make_unique<ast::NumberLit>(std::move(text), loc);
        }
        case TokenKind::StringLit:
            // строковый/байтовый литерал: значение для анализа безопасности не важно,
            // моделируем заглушкой-числом (появляется в игнорируемых позициях: emit,
            // сообщение require, data-аргумент вызова)
            advance();
            return std::make_unique<ast::NumberLit>("0", loc);
        case TokenKind::True:
            advance();
            return std::make_unique<ast::BoolLit>(true, loc);
        case TokenKind::False:
            advance();
            return std::make_unique<ast::BoolLit>(false, loc);
        case TokenKind::Identifier: {
            std::string name(peek().lexeme);
            advance();
            // new TypeName(args) — создание нового контракта. семантику развёртывания
            // не моделируем: разбираем последующее выражение (Type(args)) и возвращаем
            // как opaque (типизируется в sema как немоделируемый вызов)
            if (name == "new") return parsePrimary();
            ast::ExprPtr e = std::make_unique<ast::IdentExpr>(std::move(name), loc);
            // постфикс: .member, .call{value:...}(""), (args) — левая ассоциативность
            for (;;) {
                if (match(TokenKind::Dot)) {
                    if (!check(TokenKind::Identifier)) {
                        diag_.error(peek().loc, "expected member name after '.'");
                        return nullptr;
                    }
                    std::string m(peek().lexeme);
                    SourceLoc mloc = peek().loc;
                    advance();
                    // low-level call: target.call{value: expr}("")
                    if (m == "call" && check(TokenKind::LBrace)) {
                        advance();  // '{'
                        if (!check(TokenKind::Identifier) || peek().lexeme != "value") {
                            diag_.error(peek().loc, "expected 'value' option in low-level call");
                            return nullptr;
                        }
                        advance();  // 'value'
                        if (!expect(TokenKind::Colon, "call value option")) return nullptr;
                        auto val = parseExpr();
                        if (!val) return nullptr;
                        if (!expect(TokenKind::RBrace, "call options")) return nullptr;
                        // аргумент данных (обычно "") игнорируем — потребляем группу (...)
                        if (!check(TokenKind::LParen)) {
                            diag_.error(peek().loc, "expected '(' in low-level call");
                            return nullptr;
                        }
                        if (!skipParenGroup()) return nullptr;
                        e = std::make_unique<ast::LowLevelCallExpr>(
                            std::move(e), std::move(val), mloc);
                        continue;
                    }
                    // delegatecall / callcode: target.delegatecall[{...}]( data ) — value нет
                    if (m == "delegatecall" || m == "callcode") {
                        if (check(TokenKind::LBrace)) skipBraceBody();   // {gas:...} — игнорируем
                        if (!check(TokenKind::LParen)) {
                            diag_.error(peek().loc, "expected '(' in delegatecall");
                            return nullptr;
                        }
                        if (!skipParenGroup()) return nullptr;
                        auto zero = std::make_unique<ast::NumberLit>("0", mloc);
                        e = std::make_unique<ast::LowLevelCallExpr>(
                            std::move(e), std::move(zero), mloc, ast::LowLevelKind::DelegateCall);
                        continue;
                    }
                    // старый синтаксис: target.call.value(v)( data? )
                    if (m == "call" && check(TokenKind::Dot)) {
                        advance();  // '.'
                        if (!check(TokenKind::Identifier) || peek().lexeme != "value") {
                            diag_.error(peek().loc, "expected '.value' in low-level call");
                            return nullptr;
                        }
                        advance();  // 'value'
                        if (!expect(TokenKind::LParen, "low-level call value")) return nullptr;
                        auto val = parseExpr();
                        if (!val) return nullptr;
                        if (!expect(TokenKind::RParen, "low-level call value")) return nullptr;
                        // затем ( data? ) — аргумент данных игнорируем (в т.ч. "")
                        if (!check(TokenKind::LParen)) {
                            diag_.error(peek().loc, "expected '(' in low-level call");
                            return nullptr;
                        }
                        if (!skipParenGroup()) return nullptr;
                        e = std::make_unique<ast::LowLevelCallExpr>(
                            std::move(e), std::move(val), mloc);
                        continue;
                    }
                    e = std::make_unique<ast::MemberExpr>(std::move(e), std::move(m), mloc);
                } else if (match(TokenKind::LBracket)) {
                    // индексация mapping: base[index]
                    SourceLoc iloc = peek().loc;
                    auto idx = parseExpr();
                    if (!idx) return nullptr;
                    if (!expect(TokenKind::RBracket, "index expression")) return nullptr;
                    e = std::make_unique<ast::IndexExpr>(std::move(e), std::move(idx), iloc);
                } else if (match(TokenKind::LParen)) {
                    std::vector<ast::ExprPtr> args;
                    if (!check(TokenKind::RParen)) {
                        for (;;) {
                            auto a = parseExpr();
                            if (!a) return nullptr;
                            args.push_back(std::move(a));
                            if (!match(TokenKind::Comma)) break;
                        }
                    }
                    if (!expect(TokenKind::RParen, "call argument list")) return nullptr;
                    e = std::make_unique<ast::CallExpr>(std::move(e), std::move(args), loc);
                } else {
                    break;
                }
            }
            return e;
        }
        case TokenKind::LParen: {
            advance();
            auto inner = parseExpr();
            if (!inner) return nullptr;
            if (!expect(TokenKind::RParen, "parenthesized expression")) return nullptr;
            return inner;
        }
        default:
            diag_.error(loc, std::string("unexpected token in expression: ")
                                 + tokenKindName(peek().kind));
            return nullptr;
    }
}

int Parser::binOpPrecedence(TokenKind k) {
    switch (k) {
        case TokenKind::OrOr:                                       return 1;
        case TokenKind::AndAnd:                                     return 2;
        case TokenKind::EqEq:  case TokenKind::NotEq:               return 3;
        case TokenKind::Lt:    case TokenKind::Gt:
        case TokenKind::LtEq:  case TokenKind::GtEq:                return 4;
        case TokenKind::Plus:  case TokenKind::Minus:               return 5;
        case TokenKind::Star:  case TokenKind::Slash:
        case TokenKind::Percent:                                    return 6;
        default:                                                    return 0;
    }
}

std::optional<ast::BinOp> Parser::tokenToBinOp(TokenKind k) {
    switch (k) {
        case TokenKind::Plus:    return ast::BinOp::Add;
        case TokenKind::Minus:   return ast::BinOp::Sub;
        case TokenKind::Star:    return ast::BinOp::Mul;
        case TokenKind::Slash:   return ast::BinOp::Div;
        case TokenKind::Percent: return ast::BinOp::Mod;
        case TokenKind::EqEq:    return ast::BinOp::Eq;
        case TokenKind::NotEq:   return ast::BinOp::NotEq;
        case TokenKind::Lt:      return ast::BinOp::Lt;
        case TokenKind::Gt:      return ast::BinOp::Gt;
        case TokenKind::LtEq:    return ast::BinOp::LtEq;
        case TokenKind::GtEq:    return ast::BinOp::GtEq;
        case TokenKind::AndAnd:  return ast::BinOp::AndAnd;
        case TokenKind::OrOr:    return ast::BinOp::OrOr;
        default:                 return std::nullopt;
    }
}

ast::ExprPtr Parser::parseUnary() {
    DepthGuard g(depth_);
    if (depth_ > kMaxDepth) {
        diag_.error(peek().loc, "expression nesting too deep");
        return nullptr;
    }
    if (check(TokenKind::Not)) {
        SourceLoc loc = peek().loc;
        advance();
        auto inner = parseUnary();
        if (!inner) return nullptr;
        return std::make_unique<ast::UnaryExpr>(ast::UnOp::Not, std::move(inner), loc);
    }
    return parsePrimary();
}

ast::ExprPtr Parser::parseBinary(int minPrec) {
    auto lhs = parseUnary();
    if (!lhs) return nullptr;
    for (;;) {
        int prec = binOpPrecedence(peek().kind);
        if (prec < minPrec || prec == 0) return lhs;

        auto op = tokenToBinOp(peek().kind);
        if (!op) return lhs;
        SourceLoc loc = peek().loc;
        advance();

        auto rhs = parseBinary(prec + 1);  // левая ассоциативность
        if (!rhs) return nullptr;
        lhs = std::make_unique<ast::BinaryExpr>(*op, std::move(lhs), std::move(rhs), loc);
    }
}

ast::ExprPtr Parser::parseExpr() {
    auto cond = parseBinary(1);
    if (!cond) return nullptr;
    // тернарный оператор cond ? a : b — самый низкий приоритет, правая ассоциативность
    if (check(TokenKind::Question)) {
        SourceLoc loc = peek().loc;
        advance();
        auto thenE = parseExpr();
        if (!thenE) return nullptr;
        if (!expect(TokenKind::Colon, "ternary expression")) return nullptr;
        auto elseE = parseExpr();
        if (!elseE) return nullptr;
        return std::make_unique<ast::CondExpr>(
            std::move(cond), std::move(thenE), std::move(elseE), loc);
    }
    return cond;
}

bool Parser::parseBlockStatements(std::vector<ast::StmtPtr>& out) {
    SourceLoc openLoc = peek().loc;
    if (!expect(TokenKind::LBrace, "block")) return false;
    while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
        auto s = parseStmt();
        if (s) {
            out.push_back(std::move(s));
        } else {
            // recovery: пропустить до ';' или конца блока
            synchronizeToSemicolon();
        }
    }
    if (check(TokenKind::EndOfFile)) {
        diag_.error(openLoc, "unterminated '{' body");
        return false;
    }
    if (!expect(TokenKind::RBrace, "block")) return false;
    return true;
}

bool Parser::parseBlockOrStatement(std::vector<ast::StmtPtr>& out) {
    // тело if/else: блок { ... } либо один statement (if(cond) revert(); без скобок)
    if (check(TokenKind::LBrace)) return parseBlockStatements(out);
    auto s = parseStmt();
    if (!s) return false;
    out.push_back(std::move(s));
    return true;
}

ast::StmtPtr Parser::parseStmt() {
    DepthGuard g(depth_);
    if (depth_ > kMaxDepth) {
        diag_.error(peek().loc, "statement nesting too deep");
        return nullptr;
    }
    SourceLoc startLoc = peek().loc;

    // if (cond) { ... } [else { ... }]
    if (check(TokenKind::KwIf)) {
        advance();
        if (!expect(TokenKind::LParen, "if condition")) return nullptr;
        auto cond = parseExpr();
        if (!cond) return nullptr;
        if (!expect(TokenKind::RParen, "if condition")) return nullptr;

        std::vector<ast::StmtPtr> thenB;
        if (!parseBlockOrStatement(thenB)) return nullptr;

        std::vector<ast::StmtPtr> elseB;
        if (match(TokenKind::KwElse)) {
            if (!parseBlockOrStatement(elseB)) return nullptr;
        }

        return std::make_unique<ast::IfStmt>(
            std::move(cond), std::move(thenB), std::move(elseB), startLoc);
    }

    // return expr;
    if (check(TokenKind::KwReturn)) {
        advance();
        auto val = parseExpr();
        if (!val) return nullptr;
        if (!expect(TokenKind::Semicolon, "return statement")) return nullptr;
        return std::make_unique<ast::ReturnStmt>(std::move(val), startLoc);
    }

    // throw; — безусловный откат (старый Solidity)
    if (check(TokenKind::KwThrow)) {
        advance();
        if (!expect(TokenKind::Semicolon, "throw statement")) return nullptr;
        return std::make_unique<ast::ThrowStmt>(startLoc);
    }

    // selfdestruct(recipient); / suicide(recipient); — уничтожение контракта
    if (check(TokenKind::Identifier) &&
        (peek().lexeme == "selfdestruct" || peek().lexeme == "suicide")) {
        advance();
        if (!expect(TokenKind::LParen, "selfdestruct")) return nullptr;
        auto r = parseExpr();
        if (!r) return nullptr;
        if (!expect(TokenKind::RParen, "selfdestruct")) return nullptr;
        if (!expect(TokenKind::Semicolon, "selfdestruct")) return nullptr;
        return std::make_unique<ast::SelfdestructStmt>(std::move(r), startLoc);
    }

    // revert(); / revert("msg"); / revert CustomError(); — безусловный откат, как throw
    if (check(TokenKind::Identifier) && peek().lexeme == "revert") {
        advance();
        if (check(TokenKind::LParen)) {
            if (!skipParenGroup()) return nullptr;               // revert(...) — аргументы игнорируем
        } else {
            while (!check(TokenKind::Semicolon) && !check(TokenKind::EndOfFile)) advance();  // revert Custom()
        }
        if (!expect(TokenKind::Semicolon, "revert statement")) return nullptr;
        return std::make_unique<ast::ThrowStmt>(startLoc);
    }

    // _;  — placeholder тела модификатора (сюда подставится тело функции)
    if (check(TokenKind::Identifier) && peek().lexeme == "_") {
        advance();
        if (!expect(TokenKind::Semicolon, "modifier placeholder '_'")) return nullptr;
        return std::make_unique<ast::PlaceholderStmt>(startLoc);
    }

    // require(expr) или require(expr, "message") — сообщение игнорируем
    if (check(TokenKind::KwRequire)) {
        advance();
        if (!expect(TokenKind::LParen, "require statement")) return nullptr;
        auto cond = parseExpr();
        if (!cond) return nullptr;
        // необязательный 2-й аргумент-сообщение: пропускаем до ')'
        while (!check(TokenKind::RParen) && !check(TokenKind::EndOfFile)) advance();
        if (!expect(TokenKind::RParen, "require statement")) return nullptr;
        if (!expect(TokenKind::Semicolon, "require statement")) return nullptr;
        return std::make_unique<ast::RequireStmt>(std::move(cond), startLoc);
    }

    // emit Event(...); — события для анализа безопасности не моделируем, пропускаем
    if (check(TokenKind::Identifier) && peek().lexeme == "emit") {
        while (!check(TokenKind::Semicolon) && !check(TokenKind::EndOfFile)) advance();
        if (!expect(TokenKind::Semicolon, "emit statement")) return nullptr;
        return std::make_unique<ast::PlaceholderStmt>(startLoc);  // no-op
    }

    // кортежное присваивание: (bool success, ) = addr.call{value:x}("");
    // модерн-форма low-level call с захватом успеха. десугарим в объявление первой
    // именованной записи = результат вызова; остальные слоты кортежа игнорируем (MVP)
    if (check(TokenKind::LParen)) {
        advance();  // (
        std::optional<ast::Type> firstTy;
        std::string firstName;
        bool firstNamed = false, first = true;
        auto startsType = [&]() {
            return check(TokenKind::KwUint256) || check(TokenKind::KwBool) ||
                   check(TokenKind::KwAddress) || check(TokenKind::KwMapping);
        };
        if (!check(TokenKind::RParen)) {
            for (;;) {
                std::optional<ast::Type> ty;
                std::string nm;
                bool named = false;
                if (startsType()) { ty = parseType(); if (!ty) return nullptr; }
                if (check(TokenKind::Identifier)) { nm = std::string(peek().lexeme); advance(); named = true; }
                if (first) { firstTy = ty; firstName = nm; firstNamed = named; first = false; }
                if (!match(TokenKind::Comma)) break;
            }
        }
        if (!expect(TokenKind::RParen, "tuple assignment")) return nullptr;
        if (!expect(TokenKind::Eq, "tuple assignment")) return nullptr;
        auto val = parseExpr();
        if (!val) return nullptr;
        if (!expect(TokenKind::Semicolon, "tuple assignment")) return nullptr;

        if (firstNamed && firstTy) {
            return std::make_unique<ast::VarDeclStmt>(
                *firstTy, std::move(firstName), std::move(val), startLoc);
        }
        if (firstNamed) {
            auto tgt = std::make_unique<ast::IdentExpr>(std::move(firstName), startLoc);
            return std::make_unique<ast::AssignStmt>(std::move(tgt), std::move(val), startLoc);
        }
        return std::make_unique<ast::ExprStmt>(std::move(val), startLoc);
    }

    // var-объявление (старый Solidity): var name = expr; — тип выводится из init.
    // 'var' лексируется как идентификатор; отличаем по следующему за ним имени
    if (check(TokenKind::Identifier) && peek().lexeme == "var") {
        advance();  // 'var'
        if (!check(TokenKind::Identifier)) {
            diag_.error(peek().loc, "expected variable name after 'var'");
            return nullptr;
        }
        std::string name(peek().lexeme);
        advance();
        if (!expect(TokenKind::Eq, "var declaration")) return nullptr;
        auto init = parseExpr();
        if (!init) return nullptr;
        if (!expect(TokenKind::Semicolon, "var declaration")) return nullptr;
        // тип-заглушка (Address); реальный выведет sema, пометив inferred
        ast::Type placeholder{ast::TypeKind::Address, startLoc};
        return std::make_unique<ast::VarDeclStmt>(
            placeholder, std::move(name), std::move(init), startLoc, /*inferred=*/true);
    }

    // объявление локальной: type ident = expr;
    if (check(TokenKind::KwUint256) || check(TokenKind::KwBool) || check(TokenKind::KwAddress)) {
        auto ty = parseType();
        if (!ty) return nullptr;
        if (!check(TokenKind::Identifier)) {
            diag_.error(peek().loc, "expected variable name");
            return nullptr;
        }
        std::string name(peek().lexeme);
        advance();
        if (!expect(TokenKind::Eq, "variable declaration")) return nullptr;
        auto init = parseExpr();
        if (!init) return nullptr;
        if (!expect(TokenKind::Semicolon, "variable declaration")) return nullptr;
        return std::make_unique<ast::VarDeclStmt>(*ty, std::move(name), std::move(init), startLoc);
    }

    // начинается с идентификатора: либо присваивание (ident = expr;),
    // либо выражение-инструкция — внешний вызов (target.call{...}("");)
    if (check(TokenKind::Identifier)) {
        auto expr = parsePrimary();   // тянет постфикс: member / call / low-level call
        if (!expr) return nullptr;

        if (check(TokenKind::Eq) || check(TokenKind::PlusEq) || check(TokenKind::MinusEq)) {
            // lvalue — имя / элемент mapping (balances[k]) / поле struct (obj.field, opaque)
            if (expr->kind != ast::ExprKind::Ident && expr->kind != ast::ExprKind::Index &&
                expr->kind != ast::ExprKind::Member) {
                diag_.error(expr->loc, "invalid assignment target");
                return nullptr;
            }
            std::optional<ast::BinOp> comp;
            if (check(TokenKind::PlusEq))  comp = ast::BinOp::Add;
            if (check(TokenKind::MinusEq)) comp = ast::BinOp::Sub;
            advance();  // '=' | '+=' | '-='
            auto val = parseExpr();
            if (!val) return nullptr;
            if (!expect(TokenKind::Semicolon, "assignment")) return nullptr;
            return std::make_unique<ast::AssignStmt>(
                std::move(expr), std::move(val), startLoc, comp);
        }

        // выражение-инструкция: внешний вызов или безрезультатное выражение (напр. x;)
        if (!expect(TokenKind::Semicolon, "expression statement")) return nullptr;
        return std::make_unique<ast::ExprStmt>(std::move(expr), startLoc);
    }

    diag_.error(peek().loc, std::string("unexpected token in statement: ")
                                + tokenKindName(peek().kind));
    return nullptr;
}

}  // namespace solc
