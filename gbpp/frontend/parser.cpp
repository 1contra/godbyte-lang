#include "../include/parser.hpp"
#include <iostream>
#include <set>

namespace gbpp {

    Parser::Parser(std::vector<Token> tokens) : m_tokens(std::move(tokens)) {}

    Token Parser::peek(int offset) const {
        if (m_pos + offset >= m_tokens.size()) return m_tokens.back();
        return m_tokens[m_pos + offset];
    }

    Token Parser::advance() {
        if (!isAtEnd()) m_pos++;
        return previous();
    }

    Token Parser::previous() const {
        return m_tokens[m_pos - 1];
    }

    bool Parser::isAtEnd() const {
        return peek().type == TokenType::EndOfFile;
    }

    bool Parser::match(TokenType type) {
        if (check(type)) {
            advance();
            return true;
        }
        return false;
    }

    bool Parser::check(TokenType type) const {
        if (isAtEnd()) return false;
        return peek().type == type;
    }

    Token Parser::consume(TokenType type, const std::string& errorMsg) {
        if (check(type)) return advance();
        throw std::runtime_error("Line " + std::to_string(peek().loc.line) + ":" +
            std::to_string(peek().loc.col) + " - " + errorMsg);
    }

    void Parser::synchronize() {
        advance();
        while (!isAtEnd()) {
            if (previous().type == TokenType::Semicolon) return;
            switch (peek().type) {
                case TokenType::Struct:
                case TokenType::Enum:
                case TokenType::Alias:
                case TokenType::Fn:
                case TokenType::If:
                case TokenType::Else:
                case TokenType::While:
                case TokenType::For:
                case TokenType::Break:
                case TokenType::Return:
                case TokenType::Namespace:
                case TokenType::Asm:
                case TokenType::HashImport:
                case TokenType::RBrace:
                    return;
                default:
                    advance();
            }
        }
    }

    ParsedType Parser::parseType() {
        ParsedType pt;

        if (match(TokenType::Fn)) {
            pt.isFunction = true;
            consume(TokenType::LParen, "Expect '(' after fn");
            if (!check(TokenType::RParen)) {
                pt.paramTypes.push_back(parseType());
                while (match(TokenType::Comma)) {
                    pt.paramTypes.push_back(parseType());
                }
            }
            consume(TokenType::RParen, "Expect ')'");
            consume(TokenType::Colon, "Expect ':' after fn params");
            pt.returnType = std::make_shared<ParsedType>(parseType());
            return pt;
        }

        while (true) {
            if (match(TokenType::Owner)) {
                pt.modifiers.push_back(TypeModifier::Owner);
            }
            else if (match(TokenType::Ref)) {
                pt.modifiers.push_back(TypeModifier::Ref);
            }
            else if (match(TokenType::Volatile)) {
                pt.modifiers.push_back(TypeModifier::Volatile);
            }
            else if (match(TokenType::Const)) {
                pt.modifiers.push_back(TypeModifier::Const);
            }
            else {
                break;
            }
        }

        if (match(TokenType::U64)) pt.baseName = "u64";
        else if (match(TokenType::U32)) pt.baseName = "u32";
        else if (match(TokenType::U16)) pt.baseName = "u16";
        else if (match(TokenType::U8))  pt.baseName = "u8";
        else if (match(TokenType::I64)) pt.baseName = "i64";
        else if (match(TokenType::I32)) pt.baseName = "i32";
        else if (match(TokenType::I16)) pt.baseName = "i16";
        else if (match(TokenType::I8))  pt.baseName = "i8";
        else if (match(TokenType::F32)) pt.baseName = "f32";
        else if (match(TokenType::F64)) pt.baseName = "f64";
        else if (match(TokenType::Bool)) pt.baseName = "bool";
        else if (match(TokenType::Void)) pt.baseName = "void";
        else {
            pt.baseName = consume(TokenType::Identifier, "Expect type name").text;
            while (match(TokenType::DoubleColon)) {
                pt.baseName += "::" + consume(TokenType::Identifier, "Expect identifier").text;
            }
        }

        if (match(TokenType::LT)) {
            do {
                if (check(TokenType::IntLiteral)) {
                    ParsedType litType;
                    litType.baseName = advance().text;
                    pt.genericArgs.push_back(litType);
                }
                else {
                    pt.genericArgs.push_back(parseType());
                }
            } while (match(TokenType::Comma));
            consumeGT("Expect '>' after generic arguments");
        }

        if (match(TokenType::LBracket)) {
            pt.isArray = true;
            if (check(TokenType::IntLiteral) || check(TokenType::Identifier)) {
                pt.arraySizeExpr = advance().text;
            }
            consume(TokenType::RBracket, "Expect ']' after array bounds");
        }

        return pt;
    }

    std::string parseTypeString(Parser& p) {
        std::string typeName;

        if (p.match(TokenType::Fn)) {
            std::string typeName = "fn(";
            p.consume(TokenType::LParen, "Expect '(' after fn");
            if (!p.check(TokenType::RParen)) {
                typeName += parseTypeString(p);
                while (p.match(TokenType::Comma)) {
                    typeName += "," + parseTypeString(p);
                }
            }
            p.consume(TokenType::RParen, "Expect ')'");
            p.consume(TokenType::Colon, "Expect ':' after fn params");
            typeName += "):" + parseTypeString(p);
            return typeName;
        }

        while (true) {
            if (p.match(TokenType::Owner)) typeName += "owner ";
            else if (p.match(TokenType::Ref)) typeName += "ref ";
            else if (p.match(TokenType::Volatile)) typeName += "volatile ";
            else if (p.match(TokenType::Const)) typeName += "const ";
            else break;
        }

        if (p.match(TokenType::U64)) typeName += "u64";
        else if (p.match(TokenType::U32)) typeName += "u32";
        else if (p.match(TokenType::U16)) typeName += "u16";
        else if (p.match(TokenType::U8))  typeName += "u8";
        else if (p.match(TokenType::I64)) typeName += "i64";
        else if (p.match(TokenType::I32)) typeName += "i32";
        else if (p.match(TokenType::I16)) typeName += "i16";
        else if (p.match(TokenType::I8))  typeName += "i8";
        else if (p.match(TokenType::F32)) typeName += "f32";
        else if (p.match(TokenType::F64)) typeName += "f64";
        else if (p.match(TokenType::Void)) typeName += "void";
        else {
            typeName += p.consume(TokenType::Identifier, "Expect type name").text;
            while (p.match(TokenType::DoubleColon)) {
                typeName += "::" + p.consume(TokenType::Identifier, "Expect identifier").text;
            }
        }

        if (p.match(TokenType::LBracket)) {
            if(p.check(TokenType::IntLiteral) || p.check(TokenType::Identifier)) {
                std::string sizeStr = p.advance().text;
                p.consume(TokenType::RBracket, "Expect ']'");
                typeName += "[" + sizeStr + "]";
            }
            else {
                p.consume(TokenType::RBracket, "Expect ']'");
                typeName += "[]";
            }
        }
        return typeName;
    }

    std::unique_ptr<Program> Parser::parse() {
        auto program = std::make_unique<Program>();

        std::vector<std::string> nsStack;
        std::string currentNamespace = "";

        while (!isAtEnd()) {
            try {
                if (check(TokenType::RBrace) && !nsStack.empty()) {
                    advance();
                    currentNamespace = nsStack.back();
                    nsStack.pop_back();
                    continue;
                }

                if (match(TokenType::Namespace)) {
                    std::string nsName = consume(TokenType::Identifier, "Expect namespace name").text;
                    consume(TokenType::LBrace, "Expect '{' after namespace name");
                    nsStack.push_back(currentNamespace);
                    currentNamespace = currentNamespace.empty() ? nsName : currentNamespace + "::" + nsName;
                    continue;
                }

                std::set<std::string> pendingAttrs = parseAttributes();

                if (match(TokenType::HashImport)) {
                    auto imp = std::make_unique<ImportDecl>();
                    imp->loc = previous().loc;
                    imp->isLib = match(TokenType::Lib);
                    imp->path = consume(TokenType::StringLiteral, "Expect string literal for import path").text;
                    consume(TokenType::Semicolon, "Expect ';' after import path");
                    program->imports.push_back(std::move(imp));
                }
                else if (match(TokenType::Fn)) {
                    auto fn = parseFunction();
                    fn->attributes = std::move(pendingAttrs);
                    if (!currentNamespace.empty()) fn->name = currentNamespace + "::" + fn->name;
                    program->functions.push_back(std::move(fn));
                }
                else if (match(TokenType::Struct)) {
                    auto st = parseStruct();
                    st->attributes = std::move(pendingAttrs);
                    if (!currentNamespace.empty()) st->name = currentNamespace + "::" + st->name;
                    for (auto& m : st->methods) program->functions.push_back(std::move(m));
                    program->structs.push_back(std::move(st));
                }
                else if (match(TokenType::Enum)) {
                    auto en = parseEnum();
                    en->attributes = std::move(pendingAttrs);
                    if (!currentNamespace.empty()) en->name = currentNamespace + "::" + en->name;
                    program->enums.push_back(std::move(en));
                }
                else if (check(TokenType::Identifier) && peek(1).type == TokenType::Colon) {
                    auto decl = std::make_unique<VarDecl>();
                    decl->attributes = std::move(pendingAttrs);
                    Token nameTok = consume(TokenType::Identifier, "Expect name");
                    decl->loc = nameTok.loc;
                    decl->name = nameTok.text;
                    if (!currentNamespace.empty()) decl->name = currentNamespace + "::" + decl->name;

                    consume(TokenType::Colon, "Expect ':'");
                    decl->parsedType = parseType();

                    if (match(TokenType::Equal)) {
                        decl->initializer = parseExpression();
                    }

                    consume(TokenType::Semicolon, "Expect ';'");
                    program->globalVars.push_back(std::move(decl));
                }
                else if (match(TokenType::Alias)) {
                    auto aliasDecl = std::make_unique<AliasDecl>();
                    aliasDecl->loc = previous().loc;
                    aliasDecl->attributes = std::move(pendingAttrs);
                    aliasDecl->name = consume(TokenType::Identifier, "Expect alias name").text;
                    if (!currentNamespace.empty()) aliasDecl->name = currentNamespace + "::" + aliasDecl->name;
                    consume(TokenType::Equal, "Expect '=' after alias name");
                    aliasDecl->targetType = parseType();
                    consume(TokenType::Semicolon, "Expect ';' after alias declaration");
                    program->aliases.push_back(std::move(aliasDecl));
                }
                else {
                    throw std::runtime_error("Line " + std::to_string(peek().loc.line) + ":" +
                        std::to_string(peek().loc.col) + " - Unexpected token at global scope: '" + peek().text + "'");
                }
            }
            catch (const std::runtime_error& e) {
                this->errors.push_back(e.what());
                this->hasErrors = true;
                synchronize();
            }
        }
        return program;
    }

    std::set<std::string> Parser::parseAttributes() {
        std::set<std::string> attrs;
        if (check(TokenType::LBracket) && peek(1).type == TokenType::LBracket) {
            consume(TokenType::LBracket, ""); consume(TokenType::LBracket, "");
            while (!check(TokenType::RBracket) && !isAtEnd()) {
                if (match(TokenType::At)) {
                    attrs.insert(consume(TokenType::Identifier, "Expect attribute name").text);
                }
                else if (match(TokenType::Identifier)) {
                    std::string attrName = previous().text;
                    if (match(TokenType::Equal)) {
                        Token valTok = advance();
                        attrName += "=" + valTok.text;
                    }
                    attrs.insert(attrName);
                }

                if (match(TokenType::Comma)) continue;
                break;
            }
            consume(TokenType::RBracket, ""); consume(TokenType::RBracket, "Expect ']]'");
        }
        return attrs;
    }

    std::unique_ptr<StructDecl> Parser::parseStruct() {
        auto st = std::make_unique<StructDecl>();
        Token nameTok = consume(TokenType::Identifier, "Expect struct name");
        st->loc = nameTok.loc;
        st->name = nameTok.text;

        if (match(TokenType::LT)) {
            do {
                st->genericParams.push_back({ consume(TokenType::Identifier, "Expect generic parameter name").text });
            } while (match(TokenType::Comma));
            consumeGT("Expect '>' after generic parameters");
        }

        consume(TokenType::LBrace, "Expect '{'");

        while (!check(TokenType::RBrace) && !isAtEnd()) {
            if (check(TokenType::Identifier) && peek().text == "methods" && peek(1).type == TokenType::LBrace) {
                advance();
                consume(TokenType::LBrace, "Expect '{' after methods keyword");

                while (!check(TokenType::RBrace) && !isAtEnd()) {
                    try {
                        std::set<std::string> pendingAttrs = parseAttributes();

                        auto methodDecl = parseFunction();
                        methodDecl->attributes = std::move(pendingAttrs);
                        methodDecl->genericParams = st->genericParams;
                        methodDecl->name = st->name + "::" + methodDecl->name;

                        st->methods.push_back(std::move(methodDecl));
                    }
                    catch (const std::runtime_error& e) {
                        this->errors.push_back(e.what());
                        this->hasErrors = true;
                        synchronize();
                    }
                }
                consume(TokenType::RBrace, "Expect '}' after methods block");
                continue;
            }

            try {
                std::string name = consume(TokenType::Identifier, "Expect field name").text;
                consume(TokenType::Colon, "Expect ':'");
                ParsedType pt = parseType();

                int offset = 0;
                if (match(TokenType::At)) {
                    offset = std::stoi(consume(TokenType::IntLiteral, "Expect offset").text);
                }
                consume(TokenType::Semicolon, "Expect ';'");
                st->fields.push_back({ name, pt, offset });
            }
            catch (const std::runtime_error& e) {
                this->errors.push_back(e.what());
                this->hasErrors = true;
                synchronize();
            }
        }
        consume(TokenType::RBrace, "Expect '}'");
        consume(TokenType::Semicolon, "Expect ';'");
        return st;
    }

    std::unique_ptr<FunctionDecl> Parser::parseFunction() {
        auto fn = std::make_unique<FunctionDecl>();

        Token nameTok;
        if (check(TokenType::Identifier) || check(TokenType::Alloc) || check(TokenType::Sizeof)) {
            nameTok = advance();
        }
        else {
            nameTok = consume(TokenType::Identifier, "Expect func name");
        }

        fn->loc = nameTok.loc;
        std::string name = nameTok.text;

        if (match(TokenType::LT)) {
            do {
                fn->genericParams.push_back({ consume(TokenType::Identifier, "Expect generic parameter name").text });
            } while (match(TokenType::Comma));
            consumeGT("Expect '>' after generic parameters");
        }

        if (match(TokenType::DoubleColon)) {
            Token methodTok;
            if (check(TokenType::Identifier) || check(TokenType::Alloc) || check(TokenType::Sizeof)) {
                methodTok = advance();
            }
            else {
                methodTok = consume(TokenType::Identifier, "Expect method name");
            }
            name += "::" + methodTok.text;
        }

        fn->name = name;

        consume(TokenType::LParen, "Expect '('");
        if (!check(TokenType::RParen)) {
            do {
                std::string pName = consume(TokenType::Identifier, "Param name").text;
                consume(TokenType::Colon, "Expect ':'");
                ParsedType pt = parseType();
                fn->params.push_back({ pName, pt });
            } while (match(TokenType::Comma));
        }
        consume(TokenType::RParen, "Expect ')'");

        if (match(TokenType::Colon)) fn->returnType = parseType();
        else fn->returnType = ParsedType{ "void" };

        if (match(TokenType::Semicolon)) {
            fn->body = nullptr;
            return fn;
        }

        consume(TokenType::LBrace, "Expect '{'");
        fn->body = parseBlock();
        return fn;
    }

    std::unique_ptr<BlockStmt> Parser::parseBlock() {
        auto block = std::make_unique<BlockStmt>();
        block->loc = previous().loc;

        while (!check(TokenType::RBrace) && !isAtEnd()) {
            try {
                block->statements.push_back(parseStatement());
            }
            catch (const std::runtime_error& e) {
                this->errors.push_back(e.what());
                this->hasErrors = true;
                synchronize();
            }
        }

        consume(TokenType::RBrace, "Expect '}' after block");
        return block;
    }

    std::unique_ptr<Stmt> Parser::parseStatement() {
        std::set<std::string> pendingAttrs = parseAttributes();

        if (check(TokenType::Identifier) && peek(1).type == TokenType::Colon) {
            auto decl = std::make_unique<VarDecl>();
            decl->attributes = std::move(pendingAttrs);
            Token nameTok = consume(TokenType::Identifier, "Expect name");
            decl->loc = nameTok.loc;
            decl->name = nameTok.text;

            consume(TokenType::Colon, "Expect ':'");
            decl->parsedType = parseType();

            if (match(TokenType::Equal)) {
                decl->initializer = parseExpression();
            }

            consume(TokenType::Semicolon, "Expect ';'");
            return decl;
        }

        if (!pendingAttrs.empty()) {
            throw std::runtime_error("Attributes allowed only on variable declarations.");
        }

        if (match(TokenType::Asm)) {
            auto stmt = std::make_unique<AsmStmt>();
            stmt->loc = previous().loc;
            stmt->assembly = previous().text;
            return stmt;
        }

        if (match(TokenType::Semicolon)) return std::make_unique<BlockStmt>();

        if (match(TokenType::Return)) {
            auto ret = std::make_unique<ReturnStmt>();
            ret->loc = previous().loc;
            if (!check(TokenType::Semicolon)) ret->value = parseExpression();
            consume(TokenType::Semicolon, "Expect ';'");
            return ret;
        }
        if (match(TokenType::If)) return parseIfStatement();
        if (match(TokenType::While)) return parseWhileStatement();
        if (match(TokenType::For)) return parseForStmt();
        if (match(TokenType::Break)) {
            auto b = std::make_unique<BreakStmt>();
            b->loc = previous().loc;
            consume(TokenType::Semicolon, "Expect ';' after break");
            return b;
        }

        auto stmt = std::make_unique<ExprStmt>();
        stmt->expr = parseExpression();
        consume(TokenType::Semicolon, "Expect ';'");
        return stmt;
    }

    std::unique_ptr<Stmt> Parser::parseForStmt() {
        auto stmt = std::make_unique<ForStmt>();
        stmt->loc = previous().loc;
        consume(TokenType::LParen, "Expect '(' after 'for'");

        if (match(TokenType::Semicolon)) {
            stmt->init = nullptr;
        }
        else {
            stmt->init = parseStatement();
        }

        if (!check(TokenType::Semicolon)) {
            stmt->condition = parseExpression();
        }
        consume(TokenType::Semicolon, "Expect ';' after loop condition");

        if (!check(TokenType::RParen)) {
            stmt->update = parseExpression();
        }
        consume(TokenType::RParen, "Expect ')' after for clauses");

        consume(TokenType::LBrace, "Expect '{' before for loop body");
        stmt->body = parseBlock();

        return stmt;
    }

    std::unique_ptr<Stmt> Parser::parseIfStatement() {
        auto stmt = std::make_unique<IfStmt>();
        stmt->loc = previous().loc;
        consume(TokenType::LParen, "Expect '('");
        stmt->condition = parseExpression();
        consume(TokenType::RParen, "Expect ')'");
        consume(TokenType::LBrace, "Expect '{'");
        stmt->thenBranch = parseBlock();

        if (match(TokenType::Else)) {
            consume(TokenType::LBrace, "Expect '{' after else");
            stmt->elseBranch = parseBlock();
        }
        return stmt;
    }

    std::unique_ptr<Stmt> Parser::parseWhileStatement() {
        auto stmt = std::make_unique<WhileStmt>();
        stmt->loc = previous().loc;
        consume(TokenType::LParen, "Expect '('");
        stmt->condition = parseExpression();
        consume(TokenType::RParen, "Expect ')'");
        consume(TokenType::LBrace, "Expect '{'");
        stmt->body = parseBlock();
        return stmt;
    }

    std::unique_ptr<Expr> Parser::parseExpression() {
        return parseAssignment();
    }

    std::unique_ptr<Expr> Parser::parseAssignment() {
        auto expr = parseLogicalOr();
        if (match(TokenType::Equal) || match(TokenType::PlusEqual) ||
            match(TokenType::MinusEqual) || match(TokenType::StarEqual) ||
            match(TokenType::SlashEqual) || match(TokenType::PercentEqual)) {

            TokenType op = previous().type;
            auto assign = std::make_unique<AssignmentExpr>();
            assign->target = std::move(expr);
            assign->op = op;
            assign->value = parseAssignment();
            return assign;
        }
        return expr;
    }

    std::unique_ptr<Expr> Parser::parseLogicalOr() {
        auto expr = parseLogicalAnd();
        while (match(TokenType::PipePipe)) {
            TokenType op = previous().type;
            auto right = parseLogicalAnd();
            auto bin = std::make_unique<BinaryExpr>();
            bin->left = std::move(expr);
            bin->op = op;
            bin->right = std::move(right);
            expr = std::move(bin);
        }
        return expr;
    }

    std::unique_ptr<Expr> Parser::parseLogicalAnd() {
        auto expr = parseComparison();
        while (match(TokenType::AmpAmp)) {
            TokenType op = previous().type;
            auto right = parseComparison();
            auto bin = std::make_unique<BinaryExpr>();
            bin->left = std::move(expr);
            bin->op = op;
            bin->right = std::move(right);
            expr = std::move(bin);
        }
        return expr;
    }

    std::unique_ptr<Expr> Parser::parseComparison() {
        auto expr = parseBitwise();
        while (match(TokenType::EqualEqual) || match(TokenType::NotEqual) ||
            match(TokenType::LT) || match(TokenType::GT) ||
            match(TokenType::LE) || match(TokenType::GE)) {
            TokenType op = previous().type;
            auto right = parseBitwise();
            auto bin = std::make_unique<BinaryExpr>();
            bin->left = std::move(expr);
            bin->op = op;
            bin->right = std::move(right);
            expr = std::move(bin);
        }
        return expr;
    }

    std::unique_ptr<Expr> Parser::parseBitwise() {
        auto expr = parseShift();
        while (match(TokenType::Pipe) || match(TokenType::Ampersand) || match(TokenType::Caret)) {
            TokenType op = previous().type;
            auto right = parseShift();
            auto bin = std::make_unique<BinaryExpr>();
            bin->left = std::move(expr);
            bin->op = op;
            bin->right = std::move(right);
            expr = std::move(bin);
        }
        return expr;
    }

    std::unique_ptr<Expr> Parser::parseShift() {
        auto expr = parseTerm();
        while (match(TokenType::ShiftLeft) || match(TokenType::ShiftRight)) {
            TokenType op = previous().type;
            auto right = parseTerm();
            auto bin = std::make_unique<BinaryExpr>();
            bin->left = std::move(expr);
            bin->op = op;
            bin->right = std::move(right);
            expr = std::move(bin);
        }
        return expr;
    }

    std::unique_ptr<Expr> Parser::parseTerm() {
        auto expr = parseFactor();
        while (match(TokenType::Plus) || match(TokenType::Minus)) {
            TokenType op = previous().type;
            auto right = parseFactor();
            auto bin = std::make_unique<BinaryExpr>();
            bin->left = std::move(expr);
            bin->op = op;
            bin->right = std::move(right);
            expr = std::move(bin);
        }
        return expr;
    }

    std::unique_ptr<Expr> Parser::parseFactor() {
        auto expr = parsePrimary();

        while (match(TokenType::Star) || match(TokenType::Slash) || match(TokenType::Percent)) {
            TokenType op = previous().type;
            auto bin = std::make_unique<BinaryExpr>();
            bin->left = std::move(expr);
            bin->op = op;
            bin->right = parsePrimary();
            expr = std::move(bin);
        }
        return expr;
    }

    Token Parser::consumeGT(const std::string& errorMsg) {
        if (check(TokenType::GT)) return advance();

        if (check(TokenType::ShiftRight)) {
            m_tokens[m_pos].type = TokenType::GT;
            m_tokens[m_pos].text = ">";

            Token nextToken = m_tokens[m_pos];
            nextToken.loc.col += 1;
            m_tokens.insert(m_tokens.begin() + m_pos + 1, nextToken);

            return advance();
        }

        if (check(TokenType::GE)) {
            m_tokens[m_pos].type = TokenType::GT;
            m_tokens[m_pos].text = ">";

            Token nextToken = m_tokens[m_pos];
            nextToken.type = TokenType::Equal;
            nextToken.text = "=";
            nextToken.loc.col += 1;
            m_tokens.insert(m_tokens.begin() + m_pos + 1, nextToken);

            return advance();
        }

        throw std::runtime_error("Line " + std::to_string(peek().loc.line) + ":" +
            std::to_string(peek().loc.col) + " - " + errorMsg);
    }

    std::unique_ptr<Expr> Parser::parsePrimary() {
        std::unique_ptr<Expr> expr = nullptr;

        if (match(TokenType::StringLiteral)) {
            auto lit = std::make_unique<StringLiteral>();
            lit->value = previous().text;
            expr = std::move(lit);
        }
        else if (match(TokenType::CharLiteral)) {
            auto lit = std::make_unique<IntLiteral>();
            std::string text = previous().text;
            char c = 0;

            if (text.length() >= 3 && text[0] == '\'') {
                if (text[1] == '\\' && text.length() >= 4) {
                    if (text[2] == 'n') c = '\n';
                    else if (text[2] == 'r') c = '\r';
                    else if (text[2] == 't') c = '\t';
                    else if (text[2] == '0') c = '\0';
                    else c = text[2];
                }
                else {
                    c = text[1];
                }
            }

            lit->value = std::to_string((int)c) + "u8";
            expr = std::move(lit);
        }
        else if (match(TokenType::Minus) || match(TokenType::Bang) || match(TokenType::Tilde)) {
            auto un = std::make_unique<UnaryExpr>();
            un->loc = previous().loc;
            un->op = previous().type;
            un->operand = parsePrimary();
            expr = std::move(un);
        }
        else if (match(TokenType::Star)) {
            auto deref = std::make_unique<DerefExpr>();
            deref->operand = parsePrimary();
            expr = std::move(deref);
        }
        else if (match(TokenType::Null)) {
            expr = std::make_unique<NullLiteral>();
        }
        else if (match(TokenType::True)) {
            auto lit = std::make_unique<IntLiteral>();
            lit->loc = previous().loc;
            lit->value = "1u8";
            expr = std::move(lit);
        }
        else if (match(TokenType::False)) {
            auto lit = std::make_unique<IntLiteral>();
            lit->loc = previous().loc;
            lit->value = "0u8";
            expr = std::move(lit);
        }
        else if (match(TokenType::FloatLiteral)) {
            auto lit = std::make_unique<FloatLiteral>();
            lit->value = previous().text;
            expr = std::move(lit);
        }
        else if (match(TokenType::Sizeof)) {
            consume(TokenType::LT, "Expect '<' after sizeof");
            ParsedType pt = parseType();
            consumeGT("Expect '>' after type in sizeof");

            auto sz = std::make_unique<SizeofExpr>();
            sz->parsedTargetType = pt;
            expr = std::move(sz);
        }
        else if (match(TokenType::Alloc)) {
            consume(TokenType::LT, "Expect '<' after alloc");
            ParsedType pt = parseType();
            consumeGT("Expect '>' after type in alloc");

            consume(TokenType::LParen, "Expect '(' after alloc<T>");

            auto alloc = std::make_unique<AllocExpr>();
            alloc->parsedTargetType = pt;

            if (!check(TokenType::RParen)) {
                do { alloc->args.push_back(parseExpression()); } while (match(TokenType::Comma));
            }
            consume(TokenType::RParen, "Expect ')' after alloc args");

            expr = std::move(alloc);
        }
        else if (match(TokenType::IntLiteral)) {
            auto lit = std::make_unique<IntLiteral>();
            lit->value = previous().text;
            expr = std::move(lit);
        }
        else if (match(TokenType::Ampersand)) {
            auto addr = std::make_unique<AddrOfExpr>();
            addr->operand = parsePrimary();
            expr = std::move(addr);
        }
        else if (match(TokenType::Cast) || match(TokenType::CastBits)) {
            auto kind = (previous().type == TokenType::Cast) ? CastKind::Value : CastKind::Bits;
            consume(TokenType::LT, "Expect '<' after cast");
            ParsedType pt = parseType();
            consumeGT("Expect '>' after cast");
            consume(TokenType::LParen, "Expect '('");
            auto operand = parseExpression();
            consume(TokenType::RParen, "Expect ')'");

            auto cast = std::make_unique<CastExpr>();
            cast->castKind = kind;
            cast->parsedTargetType = pt;
            cast->operand = std::move(operand);
            expr = std::move(cast);
        }
        else if (match(TokenType::Identifier)) {
            Token identTok = previous();
            std::string name = identTok.text;

            bool isPath = false;
            while (match(TokenType::DoubleColon)) {
                isPath = true;
                name += "::" + consume(TokenType::Identifier, "Expect identifier after ::").text;
            }

            if (match(TokenType::LBrace)) {
                auto structInit = std::make_unique<StructInitExpr>();
                structInit->loc = identTok.loc;
                structInit->structName = name;

                while (!check(TokenType::RBrace) && !isAtEnd()) {
                    std::string fieldName = consume(TokenType::Identifier, "Expect field name").text;
                    consume(TokenType::Colon, "Expect ':' after field name");

                    auto initExpr = parseExpression();
                    structInit->fields.push_back({ fieldName, std::move(initExpr) });

                    if (match(TokenType::Comma)) continue;
                    break;
                }
                consume(TokenType::RBrace, "Expect '}' after struct initialization");
                expr = std::move(structInit);
            }
            else if (isPath && !check(TokenType::LParen)) {
                size_t pos = name.rfind("::");
                auto enumAcc = std::make_unique<EnumAccessExpr>();
                enumAcc->loc = identTok.loc;
                enumAcc->enumName = name.substr(0, pos);
                enumAcc->memberName = name.substr(pos + 2);
                expr = std::move(enumAcc);
            }
            else {
                auto var = std::make_unique<VarExpr>();
                var->loc = identTok.loc;
                var->name = name;
                expr = std::move(var);
            }
        }
        else if (match(TokenType::LParen)) {
            expr = parseExpression();
            consume(TokenType::RParen, "Expect ')'");
        }
        else {
            throw std::runtime_error("Line " + std::to_string(peek().loc.line) + ":" +
                std::to_string(peek().loc.col) + " - Unexpected token: '" + peek().text + "'");
        }

        while (check(TokenType::Dot) || check(TokenType::LBracket) || check(TokenType::LParen) ||
            check(TokenType::PlusPlus) || check(TokenType::MinusMinus)) {

            if (match(TokenType::PlusPlus) || match(TokenType::MinusMinus)) {
                TokenType op = previous().type;

                auto one = std::make_unique<IntLiteral>();
                one->loc = previous().loc;
                one->value = "1";

                auto assign = std::make_unique<AssignmentExpr>();
                assign->loc = previous().loc;
                assign->target = std::move(expr);
                assign->op = (op == TokenType::PlusPlus) ? TokenType::PlusEqual : TokenType::MinusEqual;
                assign->value = std::move(one);

                expr = std::move(assign);
                continue;
            }

            if (match(TokenType::Dot)) {
                auto mem = std::make_unique<MemberExpr>();
                mem->object = std::move(expr);
                mem->memberName = consume(TokenType::Identifier, "Expect member name").text;
                expr = std::move(mem);
            }
            else if (match(TokenType::LBracket)) {
                auto arr = std::make_unique<ArrayAccessExpr>();
                arr->array = std::move(expr);
                arr->index = parseExpression();
                consume(TokenType::RBracket, "Expect ']'");
                expr = std::move(arr);
            }
            else if (match(TokenType::LParen)) {
                auto call = std::make_unique<CallExpr>();
                call->loc = previous().loc;
                call->callee = std::move(expr);
                if (!check(TokenType::RParen)) {
                    do { call->args.push_back(parseExpression()); } while (match(TokenType::Comma));
                }
                consume(TokenType::RParen, "Expect ')'");
                expr = std::move(call);
            }
        }

        return expr;
    }

    std::unique_ptr<EnumDecl> Parser::parseEnum() {
        auto enm = std::make_unique<EnumDecl>();
        Token nameTok = consume(TokenType::Identifier, "Expect enum name");
        enm->loc = nameTok.loc;
        enm->name = nameTok.text;
        consume(TokenType::LBrace, "Expect '{'");

        uint64_t currentValue = 0;
        while (!check(TokenType::RBrace) && !isAtEnd()) {
            try {
                std::string memberName = consume(TokenType::Identifier, "Expect enum member").text;

                if (match(TokenType::Equal)) {
                    bool isNeg = match(TokenType::Minus);
                    if (match(TokenType::IntLiteral)) {
                        Token valTok = previous();
                        try {
                            std::string txt = valTok.text;
                            int base = (txt.starts_with("0x") || txt.starts_with("0X")) ? 16 : 10;
                            currentValue = std::stoull(txt, nullptr, base);
                            if (isNeg) currentValue = static_cast<uint64_t>(-static_cast<int64_t>(currentValue));
                        }
                        catch (const std::exception& e) {
                            throw std::runtime_error("Line " + std::to_string(valTok.loc.line) + ":" +
                                std::to_string(valTok.loc.col) + " - Invalid enum value: " + e.what());
                        }
                    }
                    else if (match(TokenType::Identifier)) {
                        throw std::runtime_error("Line " + std::to_string(peek().loc.line) + ":" +
                            std::to_string(peek().loc.col) + " - Enum member aliasing not yet supported. Use integer literals.");
                    }
                    else {
                        throw std::runtime_error("Line " + std::to_string(peek().loc.line) + ":" +
                            std::to_string(peek().loc.col) + " - Expect integer or identifier after '='");
                    }
                }

                enm->members.push_back({ memberName, currentValue });
                currentValue++;

                if (!match(TokenType::Comma)) {
                    if (!check(TokenType::RBrace)) {
                        throw std::runtime_error("Line " + std::to_string(peek().loc.line) + ":" +
                            std::to_string(peek().loc.col) + " - Expect ',' after enum member");
                    }
                }
            }
            catch (const std::runtime_error& e) {
                this->errors.push_back(e.what());
                this->hasErrors = true;
                synchronize();
            }
        }
        consume(TokenType::RBrace, "Expect '}'");
        consume(TokenType::Semicolon, "Expect ';' after enum declaration");
        return enm;
    }
}
