#pragma once
#include "token.hpp"
#include <string>
#include <vector>

namespace gbpp {

    class Lexer {
    public:
        Lexer(std::string source, std::string filename);
        std::vector<Token> tokenize();

    private:
        std::string m_src;
        std::string m_filename;
        size_t m_pos = 0;
        int m_line = 1;
        int m_col = 1;

        char peek(int offset = 0) const;
        char advance();
        bool isAtEnd() const;
        bool match(char expected);

        Token scanIdentifier();
        Token scanNumber();
        void skipWhitespace();
    };

}