#pragma once
#include "common.hpp"
#include <string>
#include <vector>

namespace gbpp {

    struct Type {
        ScalarType scalar;
        std::string name;
        int sizeBytes;
        bool isSigned = false;
        bool isFloat = false;
        bool isArray = false;

        Type* base = nullptr;

        bool isNull() const { return name == "null"; }

        std::vector<Type*> paramTypes;
        Type* returnType = nullptr;

        bool isFunction() const { return scalar == ScalarType::FunctionPtr; }

        bool isInteger() const {
            return (scalar >= ScalarType::U8 && scalar <= ScalarType::I64);
        }
        bool isFloatingPoint() const {
            return (scalar == ScalarType::F32 || scalar == ScalarType::F64);
        }
        bool isPointer() const { return scalar == ScalarType::Pointer; }

        std::string toString() const {
            if (scalar == ScalarType::Pointer && base) return "ptr<" + base->toString() + ">";
            return name;
        }

        bool operator==(const Type& other) const {
            if (this == &other) return true;

            if (this->isNull() && other.isPointer()) return true;
            if (other.isNull() && this->isPointer()) return true;

            if (scalar != other.scalar) return false;
            if (scalar == ScalarType::Struct) return name == other.name;
            if (scalar == ScalarType::Pointer) return base && other.base && *base == *other.base;
            return sizeBytes == other.sizeBytes && isSigned == other.isSigned;
        }

        bool operator!=(const Type& other) const { return !(*this == other); }
    };

    inline Type TypeVoid = { ScalarType::Void, "void", 0 };

    inline Type TypeU64 = { ScalarType::U64, "u64", 8, false };
    inline Type TypeU32 = { ScalarType::U32, "u32", 4, false };
    inline Type TypeU16 = { ScalarType::U16, "u16", 2, false };
    inline Type TypeU8 = { ScalarType::U8,  "u8",  1, false };

    inline Type TypeI64 = { ScalarType::I64, "i64", 8, true };
    inline Type TypeI32 = { ScalarType::I32, "i32", 4, true };
    inline Type TypeI16 = { ScalarType::I16, "i16", 2, true };
    inline Type TypeI8 = { ScalarType::I8,  "i8",  1, true };

    inline Type TypeF64 = { ScalarType::F64, "f64", 8, true, true };
    inline Type TypeF32 = { ScalarType::F32, "f32", 4, true, true };
    inline Type TypeBool = { ScalarType::U8, "bool", 1, false };

    inline Type TypeNull = { ScalarType::Pointer, "null", 8, false };
}