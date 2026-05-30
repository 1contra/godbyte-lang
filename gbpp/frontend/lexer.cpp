#include "../include/lexer.hpp"
#include <cctype>
#include <unordered_map>

namespace gbpp {

    Lexer::Lexer(std::string source, std::string filename)
        : m_src(std::move(source)), m_filename(std::move(filename)) {
    }

    char Lexer::peek(int offset) const {
        if (m_pos + offset >= m_src.size()) return '\0';
        return m_src[m_pos + offset];
    }

    char Lexer::advance() {
        char c = peek();
        m_pos++;
        if (c == '\n') {
            m_line++;
            m_col = 1;
        }
        else {
            m_col++;
        }
        return c;
    }

    bool Lexer::isAtEnd() const {
        return m_pos >= m_src.size();
    }

    bool Lexer::match(char expected) {
        if (isAtEnd()) return false;
        if (peek() != expected) return false;
        advance();
        return true;
    }

    void Lexer::skipWhitespace() {
        while (true) {
            char c = peek();
            if (std::isspace(c)) {
                advance();
            }
            else if (c == '/' && peek(1) == '/') {
                while (peek() != '\n' && !isAtEnd()) advance();
            }
            else if (c == '/' && peek(1) == '*') {
                advance(); advance();
                while (!isAtEnd()) {
                    if (peek() == '*' && peek(1) == '/') {
                        advance(); advance();
                        break;
                    }
                    advance();
                }
            }
            else {
                break;
            }
        }
    }

    Token Lexer::scanIdentifier() {
        SourceLoc startLoc = { m_filename, m_line, m_col };

        int startPos = m_pos;
        while (std::isalnum(peek()) || peek() == '_') {
            advance();
        }
        std::string text = m_src.substr(startPos, m_pos - startPos);

        TokenType type = TokenType::Identifier;

        static const std::unordered_map<std::string, TokenType> keywords = {
            {"namespace", TokenType::Namespace},
            {"break", TokenType::Break},
            {"fn", TokenType::Fn},
            {"asm", TokenType::Asm},
            {"lib", TokenType::Lib},
            {"return", TokenType::Return},
            {"struct", TokenType::Struct},
            {"enum", TokenType::Enum},
            {"alias", TokenType::Alias},
            {"alloc", TokenType::Alloc},
            {"sizeof", TokenType::Sizeof},
            {"owner", TokenType::Owner},
            {"ref", TokenType::Ref},
            {"null", TokenType::Null},
            {"if", TokenType::If},
            {"else", TokenType::Else},
            {"while", TokenType::While},
            {"for", TokenType::For},
            {"u8", TokenType::U8},
            {"u16", TokenType::U16},
            {"u32", TokenType::U32},
            {"u64", TokenType::U64},
            {"i8", TokenType::I8},
            {"i16", TokenType::I16},
            {"i32", TokenType::I32},
            {"i64", TokenType::I64},
            {"f32", TokenType::F32},
            {"f64", TokenType::F64},
            {"ld8", TokenType::Ld8},
            {"ld32", TokenType::Ld32},
            {"st32", TokenType::St32},
            {"true", TokenType::True},
            {"void", TokenType::Void},
            {"bool", TokenType::Bool},
            {"false", TokenType::False},
            {"cast", TokenType::Cast},
            {"cast_bits", TokenType::CastBits},
            {"const", TokenType::Const},
            {"volatile", TokenType::Volatile},
        };

        if (keywords.count(text)) {
            type = keywords.at(text);
        }

        return { type, text, startLoc };
    }

    Token Lexer::scanNumber() {
        SourceLoc loc = { m_filename, m_line, m_col };
        bool isFloat = false;

        int startPos = m_pos;

        if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
            advance(); advance();
            while (std::isxdigit(peek())) advance();
        }
        else {
            while (std::isdigit(peek())) advance();

            if (peek() == '.' && std::isdigit(peek(1))) {
                isFloat = true;
                advance();
                while (std::isdigit(peek())) advance();
            }
        }

        if (peek() == 'u' || peek() == 'f' || peek() == 'i') {
            advance();
            while (std::isdigit(peek())) advance();
        }

        std::string text = m_src.substr(startPos, m_pos - startPos);
        return { isFloat ? TokenType::FloatLiteral : TokenType::IntLiteral, text, loc };
    }

    std::vector<Token> Lexer::tokenize() {
        std::vector<Token> tokens;

        while (!isAtEnd()) {
            skipWhitespace();
            if (isAtEnd()) break;

            SourceLoc startLoc = { m_filename, m_line, m_col };
            char c = peek();

            if (std::isalpha(c) || c == '_') {
                Token tok = scanIdentifier();

                if (tok.type == TokenType::Asm) {
                    skipWhitespace();
                    if (peek() == '{') {
                        int braceDepth = 1;
                        advance();
                        std::string asmCode;
                        while (!isAtEnd() && braceDepth > 0) {
                            char c = peek();
                            if (c == '{') braceDepth++;
                            else if (c == '}') braceDepth--;

                            if (braceDepth > 0) {
                                asmCode += advance();
                            }
                            else {
                                advance();
                            }
                        }
                        tok.text = asmCode;
                    }
                }

                tokens.push_back(tok);
                continue;
            }

            if (std::isdigit(c)) {
                tokens.push_back(scanNumber());
                continue;
            }

            advance();
            switch (c) {
            case '(': tokens.push_back({ TokenType::LParen, "(", startLoc }); break;
            case ')': tokens.push_back({ TokenType::RParen, ")", startLoc }); break;
            case '{': tokens.push_back({ TokenType::LBrace, "{", startLoc }); break;
            case '}': tokens.push_back({ TokenType::RBrace, "}", startLoc }); break;
            case '[': tokens.push_back({ TokenType::LBracket, "[", startLoc }); break;
            case ']': tokens.push_back({ TokenType::RBracket, "]", startLoc }); break;
            case ';': tokens.push_back({ TokenType::Semicolon, ";", startLoc }); break;
            case ',': tokens.push_back({ TokenType::Comma, ",", startLoc }); break;
            case '+':
                if (match('+')) tokens.push_back({ TokenType::PlusPlus, "++", startLoc });
                else if (match('=')) tokens.push_back({ TokenType::PlusEqual, "+=", startLoc });
                else tokens.push_back({ TokenType::Plus, "+", startLoc });
                break;
            case '-':
                if (match('-')) tokens.push_back({ TokenType::MinusMinus, "--", startLoc });
                else if (match('=')) tokens.push_back({ TokenType::MinusEqual, "-=", startLoc });
                else tokens.push_back({ TokenType::Minus, "-", startLoc });
                break;
            case '*':
                if (match('=')) tokens.push_back({ TokenType::StarEqual, "*=", startLoc });
                else tokens.push_back({ TokenType::Star, "*", startLoc });
                break;
            case '/':
                if (match('=')) tokens.push_back({ TokenType::SlashEqual, "/=", startLoc });
                else tokens.push_back({ TokenType::Slash, "/", startLoc });
                break;
            case '%':
                if (match('=')) tokens.push_back({ TokenType::PercentEqual, "%=", startLoc });
                else tokens.push_back({ TokenType::Percent, "%", startLoc });
                break;
            case '&':
                if (match('&')) tokens.push_back({ TokenType::AmpAmp, "&&", startLoc });
                else tokens.push_back({ TokenType::Ampersand, "&", startLoc });
                break;
            case '=':
                if (match('=')) tokens.push_back({ TokenType::EqualEqual, "==", startLoc });
                else tokens.push_back({ TokenType::Equal, "=", startLoc });
                break;
            case '!':
                if (match('=')) tokens.push_back({ TokenType::NotEqual, "!=", startLoc });
                else tokens.push_back({ TokenType::Bang, "!", startLoc });
                break;
            case '@': tokens.push_back({ TokenType::At, "@", startLoc }); break;
            case '.': tokens.push_back({ TokenType::Dot, ".", startLoc }); break;
            case '~': tokens.push_back({ TokenType::Tilde, "~", startLoc }); break;
            case '^': tokens.push_back({ TokenType::Caret, "^", startLoc }); break;
            case '<':
                if (match('<')) tokens.push_back({ TokenType::ShiftLeft, "<<", startLoc });
                else if (match('=')) tokens.push_back({ TokenType::LE, "<=", startLoc });
                else tokens.push_back({ TokenType::LT, "<", startLoc });
                break;
            case '>':
                if (match('>')) tokens.push_back({ TokenType::ShiftRight, ">>", startLoc });
                else if (match('=')) tokens.push_back({ TokenType::GE, ">=", startLoc });
                else tokens.push_back({ TokenType::GT, ">", startLoc });
                break;
            case '|':
                if (match('|')) tokens.push_back({ TokenType::PipePipe, "||", startLoc });
                else tokens.push_back({ TokenType::Pipe, "|", startLoc });
                break;
            case ':':
                if (match(':')) tokens.push_back({ TokenType::DoubleColon, "::", startLoc });
                else tokens.push_back({ TokenType::Colon, ":", startLoc });
                break;
            case '"': {
                int startPos = m_pos;
                while (peek() != '"' && !isAtEnd()) {
                    advance();
                }
                std::string str = m_src.substr(startPos, m_pos - startPos);
                if (!isAtEnd()) advance();
                tokens.push_back({ TokenType::StringLiteral, str, startLoc });
                break;
            }
            case '\'': {
                int startPos = m_pos;
                while (peek() != '\'' && !isAtEnd()) {
                    if (peek() == '\\') advance();
                    advance();
                }

                std::string content = m_src.substr(startPos, m_pos - startPos);

                if (isAtEnd()) {
                    std::cerr << "Unterminated character literal at " << m_line << ":" << m_col << "\n";
                }
                else {
                    advance();
                }

                tokens.push_back({ TokenType::CharLiteral, "'" + content + "'", startLoc });
                break;
            }
            case '#': {
                if (match('i') && match('m') && match('p') && match('o') && match('r') && match('t')) {
                    tokens.push_back({ TokenType::HashImport, "#import", startLoc });
                }
                else {
                    std::cerr << "Unexpected token starting with # at " << m_line << ":" << m_col << "\n";
                }
                break;
            }
            default:
                std::cerr << "Unexpected character: " << c << " at " << m_line << ":" << m_col << "\n";
            }
        }

        tokens.push_back({ TokenType::EndOfFile, "", {m_filename, m_line, m_col} });
        return tokens;
    }

}
