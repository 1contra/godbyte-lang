#pragma once
#include "common.hpp"
#include <string>
#include <vector>
#include <iostream>

namespace gbpp {

    enum class TokenType {
        EndOfFile,
        Identifier, IntLiteral, FloatLiteral, StringLiteral, CharLiteral,
        Namespace,
        Comptime, BuiltinAllocate, Compiler,
        Variadic, Expand, Alignof,
        Asm, AsmBlock,

        HashImport, Lib,

        Fn, Return, Struct, Enum, Alias,
        Sizeof,
        Owner, Ref,
        PipePipe,
        AmpAmp,
        Null, True, False,
        If, Else, While, Break, For,

        U8, U16, U32, U64,
        I8, I16, I32, I64,
        F32, F64, Void, Bool,
        Const, Volatile,

        Ld8, Ld16, Ld32, Ld64,
        St8, St16, St32, St64,

        LParen, RParen,
        LBrace, RBrace,
        LBracket, RBracket,
        Colon, Semicolon,
        Comma,
        Equal, EqualEqual, NotEqual, Bang,
        PlusEqual, MinusEqual, StarEqual, SlashEqual, PercentEqual,
        PlusPlus, MinusMinus,
        Plus, Minus, Star, Slash, Ampersand, Percent,

        LE,
        GE,

        At,
        DoubleColon,
        Dot,
        Arrow,
        LT, GT,
        Tilde,
        Caret,
        Pipe, ShiftLeft, ShiftRight,
        BinaryExpr,
        Cast, CastBits
    };

    struct Token {
        TokenType type;
        std::string text;
        SourceLoc loc;

        std::string toString() const {
            return "Token(" + text + ")@" + std::to_string(loc.line) + ":" + std::to_string(loc.col);
        }
    };

}