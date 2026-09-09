#include "../include/parser.hpp"
#include <iostream>
#include <set>

namespace gbpp {
    Parser::Parser(std::vector<Token> tokens, TargetEnv target)
        : m_tokens(std::move(tokens)) {
        injectTargetVariables(target);
    }

    void Parser::injectTargetVariables(const TargetEnv& target) {
        m_comptimeVars["compiler.target.is_windows"] = (target.os == "windows");
        m_comptimeVars["compiler.target.is_linux"] = (target.os == "linux");
        m_comptimeVars["compiler.target.is_macos"] = (target.os == "macos");
        m_comptimeVars["compiler.target.is_wasm"] = (target.os == "wasm");

        m_comptimeVars["compiler.target.is_x86_64"] = (target.arch == "x86_64");
        m_comptimeVars["compiler.target.is_arm64"] = (target.arch == "arm64");
        m_comptimeVars["compiler.target.is_wasm32"] = (target.arch == "wasm32");

        m_comptimeVars["compiler.target.is_little_endian"] = target.isLittleEndian;
        m_comptimeVars["compiler.target.is_big_endian"] = !target.isLittleEndian;
        m_comptimeVars["compiler.target.pointer_size"] = target.pointerSize;

        m_comptimeVars["__WIN32"] = (target.os == "windows");
        m_comptimeVars["__WIN64"] = (target.os == "windows" && target.pointerSize == 8);
        m_comptimeVars["__LINUX"] = (target.os == "linux");
        m_comptimeVars["__MACOS"] = (target.os == "macos");
        m_comptimeVars["__WASM"] = (target.os == "wasm");

        m_comptimeVars["__X86_64"] = (target.arch == "x86_64");
        m_comptimeVars["__ARM64"] = (target.arch == "arm64");

        m_comptimeVars["__LITTLE_ENDIAN__"] = target.isLittleEndian;
        m_comptimeVars["__BIG_ENDIAN__"] = !target.isLittleEndian;
        m_comptimeVars["__PTR_SIZE__"] = target.pointerSize;
    }

    int64_t Parser::evaluateComptimeExpr(Expr* expr) {
        if (!expr) return 0;

        if (auto lit = dynamic_cast<IntLiteral*>(expr)) {
            try {
                std::string txt = lit->value;
                if (txt.ends_with("u8")) txt = txt.substr(0, txt.length() - 2);
                return std::stoll(txt);
            }
            catch (...) { return 0; }
        }

        if (auto hasMeth = dynamic_cast<CompilerHasMethodExpr*>(expr)) {
            return 1;
        }

        if (auto var = dynamic_cast<VarExpr*>(expr)) {
            if (m_comptimeVars.count(var->name)) {

                if (var->name.starts_with("__")) {
                    std::string diagnosticMsg = "Line " + std::to_string(var->loc.line) + ":" +
                        std::to_string(var->loc.col) +
                        " - Warning: Legacy compile macro '" + var->name +
                        "' is deprecated. Use 'compiler.target' configuration rules instead.";

                    if (this->warnings.empty() || this->warnings.back() != diagnosticMsg) {
                        this->warnings.push_back(diagnosticMsg);
                        std::cerr << diagnosticMsg << "\n";
                    }
                }

                return m_comptimeVars[var->name];
            }
            throw std::runtime_error("Line " + std::to_string(var->loc.line) + ":" +
                std::to_string(var->loc.col) +
                " - Comptime Error: Undefined configuration variable '" + var->name + "'");
        }

        if (auto ea = dynamic_cast<EnumAccessExpr*>(expr)) {
            std::string fullName = ea->enumName + "::" + ea->memberName;
            if (m_comptimeVars.count(fullName)) {
                return m_comptimeVars[fullName];
            }
            throw std::runtime_error("Line " + std::to_string(ea->loc.line) + ":" +
                std::to_string(ea->loc.col) +
                " - Comptime Error: Unresolved enum member '" + fullName + "'");
        }

        if (auto un = dynamic_cast<UnaryExpr*>(expr)) {
            int64_t val = evaluateComptimeExpr(un->operand.get());
            if (un->op == TokenType::Bang) return !val;
            if (un->op == TokenType::Minus) return -val;
        }

        if (auto bin = dynamic_cast<BinaryExpr*>(expr)) {
            int64_t l = evaluateComptimeExpr(bin->left.get());
            int64_t r = evaluateComptimeExpr(bin->right.get());
            switch (bin->op) {
            case TokenType::Plus: return l + r;
            case TokenType::Minus: return l - r;
            case TokenType::EqualEqual: return l == r;
            case TokenType::NotEqual: return l != r;
            case TokenType::AmpAmp: return l && r;
            case TokenType::PipePipe: return l || r;
            case TokenType::LT: return l < r;
            case TokenType::GT: return l > r;
            case TokenType::LE: return l <= r;
            case TokenType::GE: return l >= r;
            default: return 0;
            }
        }

        if (auto cif = dynamic_cast<ComptimeIfExpr*>(expr)) {
            int64_t condVal = evaluateComptimeExpr(cif->condition.get());
            if (condVal != 0 && cif->thenExpr) return evaluateComptimeExpr(cif->thenExpr.get());
            else if (condVal == 0 && cif->elseExpr) return evaluateComptimeExpr(cif->elseExpr.get());
            return 0;
        }

        if (auto le = dynamic_cast<LockExpr*>(expr)) {
            return evaluateComptimeExpr(le->operand.get());
        }

        return 0;
    }

    void Parser::skipBlock() {
        int depth = 1;
        while (!isAtEnd() && depth > 0) {
            if (check(TokenType::LBrace)) depth++;
            else if (check(TokenType::RBrace)) depth--;
            advance();
        }
    }

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
        ParsedType pt = parseSingleType();
        if (match(TokenType::Pipe)) {
            ParsedType unionPt;
            unionPt.isUnion = true;
            unionPt.unionTypes.push_back(pt);
            do {
                unionPt.unionTypes.push_back(parseSingleType());
            } while (match(TokenType::Pipe));
            return unionPt;
        }
        return pt;
    }

    ParsedType Parser::parseSingleType() {
        if (match(TokenType::Comptime)) {
            consume(TokenType::If, "Expect 'if' after 'comptime'");
            consume(TokenType::LParen, "Expect '('");
            auto condExpr = parseExpression();
            consume(TokenType::RParen, "Expect ')'");

            bool isTrue = evaluateComptimeExpr(condExpr.get()) != 0;
            ParsedType resultType;

            if (isTrue) {
                consume(TokenType::LBrace, "Expect '{'");
                resultType = parseType();
                consume(TokenType::RBrace, "Expect '}'");

                while (match(TokenType::Else)) {
                    if (match(TokenType::If) || check(TokenType::LParen)) {
                        consume(TokenType::LParen, "Expect '('");
                        parseExpression();
                        consume(TokenType::RParen, "Expect ')'");
                    }
                    consume(TokenType::LBrace, "Expect '{'");
                    skipBlock();
                }
            }
            else {
                consume(TokenType::LBrace, "Expect '{'");
                skipBlock();

                bool foundTrue = false;
                while (match(TokenType::Else)) {
                    if (match(TokenType::If) || check(TokenType::LParen)) {
                        consume(TokenType::LParen, "Expect '('");
                        auto elifCond = parseExpression();
                        consume(TokenType::RParen, "Expect ')'");

                        if (!foundTrue && evaluateComptimeExpr(elifCond.get()) != 0) {
                            consume(TokenType::LBrace, "Expect '{'");
                            resultType = parseType();
                            consume(TokenType::RBrace, "Expect '}'");
                            foundTrue = true;
                        }
                        else {
                            consume(TokenType::LBrace, "Expect '{'");
                            skipBlock();
                        }
                    }
                    else {
                        if (!foundTrue) {
                            consume(TokenType::LBrace, "Expect '{'");
                            resultType = parseType();
                            consume(TokenType::RBrace, "Expect '}'");
                            foundTrue = true;
                        }
                        else {
                            consume(TokenType::LBrace, "Expect '{'");
                            skipBlock();
                        }
                    }
                }
            }
            return resultType;
        }

        ParsedType pt;
        if (match(TokenType::Fn)) {
            pt.isFunction = true;
            consume(TokenType::LParen, "Expect '(' after fn");
            if (!check(TokenType::RParen)) {
                if (match(TokenType::Variadic)) {
                    pt.isVariadicFunc = true;
                }
                else {
                    pt.paramTypes.push_back(parseType());
                    while (match(TokenType::Comma)) {
                        if (match(TokenType::Variadic)) {
                            pt.isVariadicFunc = true;
                            break;
                        }
                        pt.paramTypes.push_back(parseType());
                    }
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
            typeName = "fn(";
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
            if (p.match(TokenType::Owner)) typeName += "heap ";
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
            if (p.check(TokenType::IntLiteral) || p.check(TokenType::Identifier)) {
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
                    if (nsStack.back() == "<comptime_true>") {
                        advance();
                        nsStack.pop_back();
                        while (match(TokenType::Else)) {
                            if (match(TokenType::If) || check(TokenType::LParen)) {
                                consume(TokenType::LParen, "Expect '('");
                                parseExpression();
                                consume(TokenType::RParen, "Expect ')'");
                            }
                            consume(TokenType::LBrace, "Expect '{'");
                            skipBlock();
                        }
                        if (check(TokenType::Semicolon)) advance();
                        continue;
                    }
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

                std::vector<Attribute> pendingAttrs = parseAttributes();

                bool isStandaloneSection = false;
                if (!pendingAttrs.empty() && pendingAttrs[0].kind == AttrKind::Section && !pendingAttrs[0].blockBody.empty()) {
                    isStandaloneSection = true;

                    if (check(TokenType::Fn) || check(TokenType::Struct) || check(TokenType::Enum) ||
                        check(TokenType::Alias) || check(TokenType::Namespace) || check(TokenType::HashImport) || check(TokenType::Operator)) {
                        isStandaloneSection = false;
                    }
                    if (check(TokenType::Identifier) && (peek(1).type == TokenType::Colon || peek(1).type == TokenType::Equal)) {
                        isStandaloneSection = false;
                    }
                    if (check(TokenType::Lock) || check(TokenType::Comptime)) {
                        isStandaloneSection = false;
                    }
                }

                if (isStandaloneSection) {
                    auto fn = std::make_unique<FunctionDecl>();
                    std::string secName = pendingAttrs[0].args.empty() ? ".text" : pendingAttrs[0].args[0];
                    fn->name = ".sec$" + secName + "$__standalone_" + std::to_string(m_pos);
                    fn->attributes = std::move(pendingAttrs);
                    fn->returnType = ParsedType{ "void" };

                    auto block = std::make_unique<BlockStmt>();
                    auto asmStmt = std::make_unique<AsmStmt>();

                    std::string raw = fn->attributes[0].blockBody;
                    std::string cleaned;
                    std::string currentLine;
                    char quoteChar = 0;
                    for (size_t i = 0; i < raw.length(); ++i) {
                        if (quoteChar == 0) {
                            if (raw[i] == '\'' || raw[i] == '"') quoteChar = raw[i];
                        }
                        else {
                            if (raw[i] == quoteChar && (i == 0 || raw[i - 1] != '\\')) {
                                quoteChar = 0;
                                cleaned += currentLine + "\n";
                                currentLine = "";
                            }
                            else {
                                currentLine += raw[i];
                            }
                        }
                    }
                    if (cleaned.empty()) cleaned = raw;

                    asmStmt->assembly = cleaned;
                    block->statements.push_back(std::move(asmStmt));
                    fn->body = std::move(block);
                    program->functions.push_back(std::move(fn));
                    continue;
                }

                if (check(TokenType::Identifier) && peek(1).type == TokenType::Equal) {
                    std::string varName = advance().text;
                    advance();
                    auto valExpr = parseExpression();
                    consume(TokenType::Semicolon, "Expect ';'");

                    if (m_lockedVars.count(varName)) {
                        throw std::runtime_error("Line " + std::to_string(previous().loc.line) + ":" + std::to_string(previous().loc.col) + " - Comptime Error: Cannot reassign locked variable '" + varName + "'");
                    }

                    bool isLocking = false;
                    Expr* evExpr = valExpr.get();
                    if (auto le = dynamic_cast<LockExpr*>(evExpr)) {
                        isLocking = true;
                        evExpr = le->operand.get();
                    }

                    m_comptimeVars[varName] = evaluateComptimeExpr(evExpr);
                    if (isLocking) m_lockedVars.insert(varName);
                    continue;
                }

                if (match(TokenType::Lock)) {
                    std::string varName = consume(TokenType::Identifier, "Expect var after lock").text;
                    consume(TokenType::Semicolon, "Expect ';'");
                    m_lockedVars.insert(varName);
                    continue;
                }

                if (match(TokenType::Comptime)) {
                    if (match(TokenType::If)) {
                        consume(TokenType::LParen, "Expect '('");
                        auto condExpr = parseExpression();
                        consume(TokenType::RParen, "Expect ')'");

                        bool isTrue = evaluateComptimeExpr(condExpr.get()) != 0;
                        if (isTrue) {
                            consume(TokenType::LBrace, "Expect '{'");
                            nsStack.push_back("<comptime_true>");
                        }
                        else {
                            consume(TokenType::LBrace, "Expect '{'");
                            skipBlock();

                            bool foundTrue = false;
                            while (match(TokenType::Else)) {
                                if (match(TokenType::If) || check(TokenType::LParen)) {
                                    consume(TokenType::LParen, "Expect '('");
                                    auto elifCond = parseExpression();
                                    consume(TokenType::RParen, "Expect ')'");

                                    if (!foundTrue && evaluateComptimeExpr(elifCond.get()) != 0) {
                                        consume(TokenType::LBrace, "Expect '{'");
                                        nsStack.push_back("<comptime_true>");
                                        foundTrue = true;
                                        break;
                                    }
                                    else {
                                        consume(TokenType::LBrace, "Expect '{'");
                                        skipBlock();
                                    }
                                }
                                else {
                                    if (!foundTrue) {
                                        consume(TokenType::LBrace, "Expect '{'");
                                        nsStack.push_back("<comptime_true>");
                                        foundTrue = true;
                                        break;
                                    }
                                    else {
                                        consume(TokenType::LBrace, "Expect '{'");
                                        skipBlock();
                                    }
                                }
                            }
                        }
                        continue;
                    }
                }

                if (match(TokenType::HashImport)) {
                    auto imp = std::make_unique<ImportDecl>();
                    imp->loc = previous().loc;
                    imp->isLib = match(TokenType::Lib);
                    imp->path = consume(TokenType::StringLiteral, "Expect string literal for import path").text;
                    consume(TokenType::Semicolon, "Expect ';' after import path");
                    program->imports.push_back(std::move(imp));
                }
                else if (match(TokenType::Operator)) {
                    auto fn = parseOperator();
                    fn->attributes = std::move(pendingAttrs);
                    if (!currentNamespace.empty()) fn->name = currentNamespace + "::" + fn->name;
                    program->functions.push_back(std::move(fn));
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

                    if (hasAttribute(en->attributes, AttrKind::Config)) {
                        for (const auto& member : en->members) {
                            m_comptimeVars[en->name + "::" + member.name] = (int64_t)member.value;
                        }
                    }
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

                    if (hasAttribute(decl->attributes, AttrKind::Config)){
                        int64_t val = 0;
                        bool isLocking = hasAttribute(decl->attributes, AttrKind::Lockable) || hasAttribute(decl->attributes, AttrKind::Locked);
                        Expr* evExpr = decl->initializer.get();
                        if (evExpr) {
                            if (auto le = dynamic_cast<LockExpr*>(evExpr)) {
                                isLocking = true;
                                evExpr = le->operand.get();
                            }
                            val = evaluateComptimeExpr(evExpr);
                        }
                        m_comptimeVars[decl->name] = val;
                        if (isLocking) m_lockedVars.insert(decl->name);
                    }
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

    AttrKind stringToAttrKind(const std::string& str) {
        if (str == "addr") return AttrKind::Addr;
        if (str == "deprecated") return AttrKind::Deprecated;
        if (str == "section") return AttrKind::Section;
        if (str == "inline") return AttrKind::Inline;
        if (str == "export") return AttrKind::Export;
        if (str == "extern") return AttrKind::Extern;
        if (str == "config") return AttrKind::Config;
        if (str == "nofree") return AttrKind::Nofree;
        if (str == "lockable") return AttrKind::Lockable;
        if (str == "locked") return AttrKind::Locked;
        if (str == "free") return AttrKind::Free;
        return AttrKind::Unknown;
    }

    std::vector<Attribute> Parser::parseAttributes() {
        std::vector<Attribute> attrs;
        if (check(TokenType::LBracket) && peek(1).type == TokenType::LBracket) {
            consume(TokenType::LBracket, ""); consume(TokenType::LBracket, "");
            while (!check(TokenType::RBracket) && !isAtEnd()) {
                Attribute attr;

                if (match(TokenType::At)) {
                    attr.rawName = consume(TokenType::Identifier, "Expect attribute name").text;
                }
                else if (match(TokenType::Identifier)) {
                    attr.rawName = previous().text;
                }

                attr.kind = stringToAttrKind(attr.rawName);

                if (match(TokenType::Equal)) {
                    attr.args.push_back(advance().text);
                }

                if (match(TokenType::LParen)) {
                    while (!check(TokenType::RParen) && !isAtEnd()) {
                        std::string argStr;
                        while (!check(TokenType::Comma) && !check(TokenType::RParen) && !isAtEnd()) {
                            argStr += advance().text;
                        }
                        if (!argStr.empty()) attr.args.push_back(argStr);
                        if (match(TokenType::Comma)) continue;
                    }
                    consume(TokenType::RParen, "Expect ')' after attribute arguments");
                }

                if (match(TokenType::LBrace)) {
                    int braceDepth = 1;
                    std::string blockContent = "";
                    while (!isAtEnd() && braceDepth > 0) {
                        Token t = advance();
                        if (t.type == TokenType::LBrace) braceDepth++;
                        else if (t.type == TokenType::RBrace) braceDepth--;

                        if (braceDepth > 0) {
                            blockContent += t.text + (t.type == TokenType::StringLiteral ? "" : " ");
                        }
                    }
                    attr.blockBody = blockContent;
                }

                attrs.push_back(attr);

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
                        std::vector<Attribute> pendingAttrs = parseAttributes();
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
                std::vector<Attribute> fieldAttrs = parseAttributes();
                std::string fieldName = consume(TokenType::Identifier, "Expect field name").text;
                consume(TokenType::Colon, "Expect ':'");
                ParsedType pt = parseType();
                int offset = 0;
                if (match(TokenType::At)) {
                    offset = std::stoi(consume(TokenType::IntLiteral, "Expect offset").text);
                }
                consume(TokenType::Semicolon, "Expect ';'");
                st->fields.push_back({ fieldName, pt, offset, std::move(fieldAttrs) });
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
        if (check(TokenType::Identifier) || check(TokenType::Sizeof)) {
            nameTok = advance();
        }
        else {
            nameTok = consume(TokenType::Identifier, "Expect func name");
        }
        fn->loc = nameTok.loc;
        std::string fnName = nameTok.text;

        if (match(TokenType::LT)) {
            do {
                bool isVar = match(TokenType::Variadic);
                std::string paramName = consume(TokenType::Identifier, "Expect generic parameter name").text;
                fn->genericParams.push_back({ paramName, isVar });
            } while (match(TokenType::Comma));
            consumeGT("Expect '>' after generic parameters");
        }

        if (match(TokenType::DoubleColon)) {
            Token methodTok;
            if (check(TokenType::Identifier) || check(TokenType::Sizeof)) {
                methodTok = advance();
            }
            else {
                methodTok = consume(TokenType::Identifier, "Expect method name");
            }
            fnName += "::" + methodTok.text;
        }
        fn->name = fnName;

        consume(TokenType::LParen, "Expect '('");
        if (!check(TokenType::RParen)) {
            do {
                if (match(TokenType::Variadic)) {
                    fn->isVariadic = true;
                    std::string pName = consume(TokenType::Identifier, "Expect param name after variadic").text;
                    ParsedType varType; varType.baseName = "void";
                    fn->params.push_back({ pName, varType, nullptr, {} });
                    break;
                }
                std::vector<Attribute> paramAttrs = parseAttributes();
                std::string pName = consume(TokenType::Identifier, "Param name").text;
                consume(TokenType::Colon, "Expect ':'");
                ParsedType pt = parseType();
                fn->params.push_back({ pName, pt, nullptr, std::move(paramAttrs) });
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

    std::unique_ptr<FunctionDecl> Parser::parseOperator() {
        auto fn = std::make_unique<FunctionDecl>();
        fn->loc = previous().loc;
        Token structTok = consume(TokenType::Identifier, "Expect struct name for operator");
        std::string structName = structTok.text;

        if (match(TokenType::LT)) {
            do {
                fn->genericParams.push_back({ consume(TokenType::Identifier, "Expect generic parameter").text });
            } while (match(TokenType::Comma));
            consumeGT("Expect '>'");
        }

        consume(TokenType::DoubleColon, "Expect '::' after struct name");

        fn->parentStructName = structName;
        fn->isOperator = true;

        std::string opName = "operator";
        if (match(TokenType::LBracket)) {
            consume(TokenType::RBracket, "Expect ']' for operator[]");
            opName += "[]";
            fn->operatorKind = TokenType::LBracket;
        }
        else if (match(TokenType::Plus)) { opName += "+"; fn->operatorKind = TokenType::Plus; }
        else if (match(TokenType::Minus)) { opName += "-"; fn->operatorKind = TokenType::Minus; }
        else if (match(TokenType::Star)) { opName += "*"; fn->operatorKind = TokenType::Star; }
        else if (match(TokenType::Slash)) { opName += "/"; fn->operatorKind = TokenType::Slash; }
        else if (match(TokenType::Percent)) { opName += "%"; fn->operatorKind = TokenType::Percent; }
        else if (match(TokenType::EqualEqual)) { opName += "=="; fn->operatorKind = TokenType::EqualEqual; }
        else if (match(TokenType::NotEqual)) { opName += "!="; fn->operatorKind = TokenType::NotEqual; }
        else if (match(TokenType::LT)) { opName += "<"; fn->operatorKind = TokenType::LT; }
        else if (match(TokenType::GT)) { opName += ">"; fn->operatorKind = TokenType::GT; }
        else if (match(TokenType::LE)) { opName += "<="; fn->operatorKind = TokenType::LE; }
        else if (match(TokenType::GE)) { opName += ">="; fn->operatorKind = TokenType::GE; }
        else throw std::runtime_error("Unsupported operator overloading token");

        fn->name = structName + "::" + opName;

        consume(TokenType::LParen, "Expect '('");

        ParsedType thisType;
        thisType.baseName = structName;
        thisType.modifiers.push_back(TypeModifier::Ref);
        for (auto& gp : fn->genericParams) {
            ParsedType genArg; genArg.baseName = gp.name;
            thisType.genericArgs.push_back(genArg);
        }
        fn->params.push_back({ "this", thisType, nullptr, {} });

        if (!check(TokenType::RParen)) {
            do {
                std::vector<Attribute> paramAttrs = parseAttributes();
                std::string pName = consume(TokenType::Identifier, "Param name").text;
                consume(TokenType::Colon, "Expect ':'");
                ParsedType pt = parseType();
                fn->params.push_back({ pName, pt, nullptr, std::move(paramAttrs) });
            } while (match(TokenType::Comma));
        }
        consume(TokenType::RParen, "Expect ')'");

        if (match(TokenType::Colon)) fn->returnType = parseType();
        else fn->returnType = ParsedType{ "void" };

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
        std::vector<Attribute> pendingAttrs = parseAttributes();
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
        if (match(TokenType::Comptime)) {
            auto cIf = std::make_unique<ComptimeIfStmt>();
            cIf->loc = previous().loc;
            consume(TokenType::If, "Expect 'if' after 'comptime'");
            consume(TokenType::LParen, "Expect '(' after comptime if");
            cIf->condition = parseExpression();
            consume(TokenType::RParen, "Expect ')' after condition");
            consume(TokenType::LBrace, "Expect '{' for comptime if block");
            cIf->thenBranch = parseBlock();

            ComptimeIfStmt* currentIf = cIf.get();
            while (match(TokenType::Else)) {
                if (match(TokenType::If)) {
                    auto nextIf = std::make_unique<ComptimeIfStmt>();
                    nextIf->loc = previous().loc;
                    consume(TokenType::LParen, "Expect '('");
                    nextIf->condition = parseExpression();
                    consume(TokenType::RParen, "Expect ')'");
                    consume(TokenType::LBrace, "Expect '{'");
                    nextIf->thenBranch = parseBlock();

                    auto temp = nextIf.get();
                    currentIf->elseBranch = std::move(nextIf);
                    currentIf = temp;
                }
                else {
                    consume(TokenType::LBrace, "Expect '{' after else");
                    currentIf->elseBranch = parseBlock();
                    break;
                }
            }
            return cIf;
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
        if (match(TokenType::Continue)) {
            auto c = std::make_unique<ContinueStmt>();
            c->loc = previous().loc;
            consume(TokenType::Semicolon, "Expect ';' after continue");
            return c;
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
            if (match(TokenType::If)) {
                stmt->elseBranch = parseIfStatement();
            }
            else {
                consume(TokenType::LBrace, "Expect '{' after else");
                stmt->elseBranch = parseBlock();
            }
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
        else if (match(TokenType::BuiltinMemfill) || match(TokenType::BuiltinMemcpy) ||
            match(TokenType::BuiltinTrap) || match(TokenType::BuiltinBswap) ||
            match(TokenType::BuiltinUnreachable) || match(TokenType::BuiltinAllocate)) {

            auto builtin = std::make_unique<BuiltinCallExpr>();
            builtin->loc = previous().loc;
            builtin->builtinType = previous().type;

            consume(TokenType::LParen, "Expect '(' after builtin");
            if (!check(TokenType::RParen)) {
                do {
                    builtin->args.push_back(parseExpression());
                } while (match(TokenType::Comma));
            }
            consume(TokenType::RParen, "Expect ')' after builtin arguments");
            expr = std::move(builtin);
        }
        else if (match(TokenType::Lock)) {
            auto l = std::make_unique<LockExpr>();
            l->loc = previous().loc;
            l->operand = parsePrimary();
            expr = std::move(l);
        }
        else if (match(TokenType::Comptime)) {
            consume(TokenType::If, "Expect 'if'");
            consume(TokenType::LParen, "Expect '('");
            auto condExpr = parseExpression();
            consume(TokenType::RParen, "Expect ')'");

            bool isTrue = evaluateComptimeExpr(condExpr.get()) != 0;
            std::unique_ptr<Expr> resultExpr = nullptr;

            if (isTrue) {
                consume(TokenType::LBrace, "Expect '{'");
                resultExpr = parseExpression();
                if (check(TokenType::Semicolon) && peek(1).type == TokenType::RBrace) advance();
                consume(TokenType::RBrace, "Expect '}'");

                while (match(TokenType::Else)) {
                    if (match(TokenType::If) || check(TokenType::LParen)) {
                        consume(TokenType::LParen, "Expect '('");
                        parseExpression();
                        consume(TokenType::RParen, "Expect ')'");
                    }
                    consume(TokenType::LBrace, "Expect '{'");
                    skipBlock();
                }
            }
            else {
                consume(TokenType::LBrace, "Expect '{'");
                skipBlock();

                bool foundTrue = false;
                while (match(TokenType::Else)) {
                    if (match(TokenType::If) || check(TokenType::LParen)) {
                        consume(TokenType::LParen, "Expect '('");
                        auto elifCond = parseExpression();
                        consume(TokenType::RParen, "Expect ')'");

                        if (!foundTrue && evaluateComptimeExpr(elifCond.get()) != 0) {
                            consume(TokenType::LBrace, "Expect '{'");
                            resultExpr = parseExpression();
                            if (check(TokenType::Semicolon) && peek(1).type == TokenType::RBrace) advance();
                            consume(TokenType::RBrace, "Expect '}'");
                            foundTrue = true;
                        }
                        else {
                            consume(TokenType::LBrace, "Expect '{'");
                            skipBlock();
                        }
                    }
                    else {
                        if (!foundTrue) {
                            consume(TokenType::LBrace, "Expect '{'");
                            resultExpr = parseExpression();
                            if (check(TokenType::Semicolon) && peek(1).type == TokenType::RBrace) advance();
                            consume(TokenType::RBrace, "Expect '}'");
                            foundTrue = true;
                        }
                        else {
                            consume(TokenType::LBrace, "Expect '{'");
                            skipBlock();
                        }
                    }
                }
            }
            if (!resultExpr) resultExpr = std::make_unique<NullLiteral>();
            expr = std::move(resultExpr);
        }
        else if (match(TokenType::Compiler)) {
            expr = parseCompilerIntrinsic();
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
        else if (match(TokenType::Alignof)) {
            consume(TokenType::LT, "Expect '<' after alignof");
            ParsedType pt = parseType();
            consumeGT("Expect '>' after type in alignof");
            auto al = std::make_unique<AlignofExpr>();
            al->parsedTargetType = pt;
            expr = std::move(al);
        }
        else if (match(TokenType::Expand)) {
            consume(TokenType::LParen, "Expect '(' after expand");
            auto exp = std::make_unique<ExpandExpr>();
            exp->loc = previous().loc;
            exp->operand = parseExpression();
            consume(TokenType::RParen, "Expect ')'");
            expr = std::move(exp);
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
            std::vector<ParsedType> genArgs;
            if (check(TokenType::LT)) {
                size_t savedPos = m_pos;
                try {
                    advance();
                    do {
                        genArgs.push_back(parseType());
                    } while (match(TokenType::Comma));
                    consumeGT("");
                }
                catch (...) {
                    m_pos = savedPos;
                    genArgs.clear();
                }
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
                var->genericArgs = genArgs;
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
            check(TokenType::PlusPlus) || check(TokenType::MinusMinus) || check(TokenType::Arrow)) {
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
                Token memberTok = consume(TokenType::Identifier, "Expect member name");
                mem->loc = memberTok.loc;
                mem->memberName = memberTok.text;
                expr = std::move(mem);
            }
            else if (match(TokenType::Arrow)) {
                auto deref = std::make_unique<DerefExpr>();
                deref->loc = previous().loc;
                deref->operand = std::move(expr);
                auto mem = std::make_unique<MemberExpr>();
                mem->object = std::move(deref);
                Token memberTok = consume(TokenType::Identifier, "Expect member name");
                mem->loc = memberTok.loc;
                mem->memberName = memberTok.text;
                expr = std::move(mem);
            }
            else if (match(TokenType::LBracket)) {
                auto arr = std::make_unique<ArrayAccessExpr>();
                arr->loc = expr->loc;
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

    std::unique_ptr<Expr> Parser::parseCompilerIntrinsic() {
        Token startTok = previous();
        consume(TokenType::Dot, "Expect '.' after 'compiler' keyword");
        Token intrinsicName = consume(TokenType::Identifier, "Expect compiler intrinsic name");
        if (intrinsicName.text == "has_method") {
            auto hasMeth = std::make_unique<CompilerHasMethodExpr>();
            hasMeth->loc = startTok.loc;
            consume(TokenType::LParen, "Expect '(' after compiler.has_method");
            hasMeth->parsedTargetType = parseType();
            consume(TokenType::Comma, "Expect ','");
            std::string rawStr = consume(TokenType::StringLiteral, "Expect string literal for method name").text;
            hasMeth->methodName = rawStr.substr(1, rawStr.length() - 2);
            consume(TokenType::RParen, "Expect ')'");
            return hasMeth;
        }
        throw std::runtime_error("Line " + std::to_string(intrinsicName.loc.line) + ":" +
            std::to_string(intrinsicName.loc.col) + " - Unknown compiler intrinsic: '" + intrinsicName.text + "'");
    }

    std::unique_ptr<EnumDecl> Parser::parseEnum() {
        auto enm = std::make_unique<EnumDecl>();
        Token nameTok = consume(TokenType::Identifier, "Expect enum name");
        enm->loc = nameTok.loc;
        enm->name = nameTok.text;

        if (match(TokenType::Colon)) {
            enm->underlyingType = parseType();
        }
        else {
            enm->underlyingType.baseName = "u64";
        }

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