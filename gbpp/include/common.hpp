#pragma once
#include <string>
#include <cstdint>

namespace gbpp {

    struct SourceLoc {
        std::string filename;
        int line;
        int col;
    };

    enum class ScalarType {
        Void,
        U8, U16, U32, U64,
        I8, I16, I32, I64,
        F32, F64,
        Pointer,
        FunctionPtr,
        Struct,
        Unknown
    };
}