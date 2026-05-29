#pragma once
#include "token.hpp"
#include "ast.hpp"
#include <vector>
#include <memory>

namespace gbpp {

    class Parser {
    public:
        Parser(std::vector<Token> tokens);
        std::unique_ptr<Program> parse();

        bool hasErrors = false;
        std::vector<std::string> errors;

        bool match(TokenType type);
        Token consume(TokenType type, const std::string& errorMsg);
        Token consumeGT(const std::string& errorMsg);
        Token advance();

        bool check(TokenType type) const;

    private:
        std::vector<Token> m_tokens;
        size_t m_pos = 0;

        Token peek(int offset = 0) const;
        Token previous() const;
        bool isAtEnd() const;
        ParsedType parseType();

        void synchronize();

        std::set<std::string> parseAttributes();

        std::unique_ptr<EnumDecl> parseEnum();

        std::unique_ptr<FunctionDecl> parseFunction();
        std::unique_ptr<StructDecl> parseStruct();

        std::unique_ptr<BlockStmt> parseBlock();
        std::unique_ptr<Stmt> parseStatement();

        std::unique_ptr<Stmt> parseIfStatement();
        std::unique_ptr<Stmt> parseWhileStatement();
        std::unique_ptr<Stmt> parseForStmt();

        std::unique_ptr<Expr> parseLogicalOr();
        std::unique_ptr<Expr> parseLogicalAnd();

        std::unique_ptr<Expr> parseExpression();
        std::unique_ptr<Expr> parseAssignment();
        std::unique_ptr<Expr> parseComparison();
        std::unique_ptr<Expr> parseTerm();
        std::unique_ptr<Expr> parseFactor();
        std::unique_ptr<Expr> parsePrimary();
        std::unique_ptr<Expr> parseBitwise();
        std::unique_ptr<Expr> parseShift();
    };
}