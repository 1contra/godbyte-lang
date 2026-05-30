#pragma once
#include "common.hpp"
#include <string>
#include <vector>
#include <iostream>
#include <sstream>

namespace gbpp {

    enum class OpCode {
        GET_PARAM,
        CONST, MOV,
        LEA_LOCAL,
        LOAD_LOCAL, STORE_LOCAL,
        LOAD, STORE,
        ALLOC, AND,
        ADD, SUB, MUL, DIV,
        FADD, FSUB, FMUL, FDIV,
        RET, XOR,
        LABEL, JMP, JMP_FALSE, CMP_EQ, CMP_LT,
        CAST, ZEXT, TRUNC, CALL, LOAD_STR,
        CMP_NE, CMP_GT, CMP_GE, CMP_LE,
        INLINE_ASM,
        OR, SHL, SHR
    };

    struct ExprVal {
        OpCode op;
        int src1;
        int src2;
        uint64_t imm;
        bool operator<(const ExprVal& o) const {
            if (op != o.op) return op < o.op;
            if (src1 != o.src1) return src1 < o.src1;
            if (src2 != o.src2) return src2 < o.src2;
            return imm < o.imm;
        }
    };

    struct Instruction {
        OpCode op;
        int dest = -1;
        int src1 = -1;
        int src2 = -1;
        uint64_t imm = 0;
        int bytes = 8;
        ScalarType type = ScalarType::Void;
        std::string label;

        std::vector<int> args;
        std::vector<int> argBytes;

        std::string getType() const {
            if (bytes == 1) return "i8";
            if (bytes == 2) return "i16";
            if (bytes == 4) return "i32";
            if (bytes == 8) return "i64";
            return "void";
        }

        std::string toString() const {
            using std::to_string;
            std::string typeStr = getType();

            auto op2 = [&]() {
                return (src2 == -1) ? to_string(imm) : "v" + to_string(src2);
            };

            auto destStr = [&]() {
                return (dest == -1) ? "" : "v" + to_string(dest) + " := ";
            };

            auto formatArgs = [&](const std::vector<int>& args) -> std::string {
                if (args.empty()) return "";
                std::string out = "(";
                for (size_t i = 0; i < args.size(); ++i)
                    out += (i ? ", " : "") + std::string("v") + std::to_string(args[i]);
                out += ")";
                return out;
            };

            switch (op) {
                case OpCode::GET_PARAM:
                    return destStr() + "param " + typeStr + " [" + to_string(imm) + "]";
                case OpCode::CONST:
                    return destStr() + "const " + typeStr + " " + to_string(imm);
                case OpCode::MOV:
                    return destStr() + "mov " + typeStr + " v" + to_string(src1);
                case OpCode::ADD:
                    return destStr() + "add " + typeStr + " v" + to_string(src1) + ", " + op2();
                case OpCode::SUB:
                    return destStr() + "sub " + typeStr + " v" + to_string(src1) + ", " + op2();
                case OpCode::MUL:
                    return destStr() + "mul " + typeStr + " v" + to_string(src1) + ", " + op2();
                case OpCode::DIV:
                    return destStr() + "div " + typeStr + " v" + to_string(src1) + ", v" + to_string(src2);
                case OpCode::RET:
                    if (src1 != -1) return "ret " + typeStr + " v" + to_string(src1);
                    return "ret void";
                case OpCode::LOAD:
                    return destStr() + "load " + typeStr + " from [v" + to_string(src1) + "]";
                case OpCode::STORE:
                    return "store " + getType() + " " + op2() + " into [v" + to_string(src1) + "]";
                case OpCode::ALLOC:
                    return destStr() + "alloc " + to_string(imm) + " bytes";
                case OpCode::CAST:
                    return destStr() + "cast v" + to_string(src1) + " to " + typeStr;
                case OpCode::ZEXT:
                    return destStr() + "zext v" + to_string(src1) + " to " + typeStr;
                case OpCode::TRUNC:
                    return destStr() + "trunc v" + to_string(src1) + " to " + typeStr;
                case OpCode::CALL:
                    return destStr() + "call @" + label + formatArgs(args);
                case OpCode::AND:
                    return destStr() + "and " + typeStr + " v" + to_string(src1) + ", " + op2();
                case OpCode::JMP: {
                    std::string res = "jmp " + (label.empty() ? ".L" + std::to_string(imm) : label);
                    return res + formatArgs(args);
                }

                case OpCode::JMP_FALSE: {
                    std::string res = "jmp_false v" + std::to_string(src1)
                        + " -> .L" + std::to_string(imm);
                    return res + formatArgs(args);
                }
                case OpCode::LABEL:
                    return ".L" + to_string(imm) + ":";
                case OpCode::CMP_LT:
                    return destStr() + "cmp_lt " + typeStr + " v" + to_string(src1) + ", " + op2();
                case OpCode::CMP_EQ:
                    return destStr() + "cmp_eq " + typeStr + " v" + to_string(src1) + ", " + op2();
                case OpCode::CMP_NE:
                    return destStr() + "cmp_ne " + typeStr + " v" + to_string(src1) + ", " + op2();
                case OpCode::CMP_GT:
                    return destStr() + "cmp_gt " + typeStr + " v" + to_string(src1) + ", " + op2();
                case OpCode::CMP_GE:
                    return destStr() + "cmp_ge " + typeStr + " v" + to_string(src1) + ", " + op2();
                case OpCode::CMP_LE:
                    return destStr() + "cmp_le " + typeStr + " v" + to_string(src1) + ", " + op2();
                case OpCode::LOAD_LOCAL:
                    return destStr() + "load_local " + typeStr + " local[" + std::to_string(imm) + "]";
                case OpCode::STORE_LOCAL:
                    return "store_local " + typeStr + " v" + std::to_string(src1) + " -> local[" + std::to_string(imm) + "]";
                case OpCode::LOAD_STR:
                    return destStr() + "load_str [rel " + label + "]";
                case OpCode::OR:
                    return destStr() + "or " + typeStr + " v" + to_string(src1) + ", " + op2();
                case OpCode::SHL:
                    return destStr() + "shl " + typeStr + " v" + to_string(src1) + ", " + op2();
                case OpCode::SHR:
                    return destStr() + "shr " + typeStr + " v" + to_string(src1) + ", " + op2();
                case OpCode::INLINE_ASM:
                    return "; inline_asm: " + label;
                case OpCode::XOR:
                    return destStr() + "xor " + typeStr + " v" + to_string(src1) + ", " + op2();
                case OpCode::LEA_LOCAL:
                    return destStr() + "lea_local " + typeStr + " local[" + std::to_string(imm) + "]";
                case OpCode::FADD:
                    return destStr() + "fadd " + typeStr + " v" + to_string(src1) + ", " + op2();
                case OpCode::FSUB:
                    return destStr() + "fsub " + typeStr + " v" + to_string(src1) + ", " + op2();
                case OpCode::FMUL:
                    return destStr() + "fmul " + typeStr + " v" + to_string(src1) + ", " + op2();
                case OpCode::FDIV:
                    return destStr() + "fdiv " + typeStr + " v" + to_string(src1) + ", v" + to_string(src2);
                default:
                    return "; Unknown Op " + to_string((int)op);
            }
        }
    };

    struct BasicBlock {
        int id;
        std::string name;
        std::vector<int> params;
        std::vector<Instruction> instructions;
    };

    struct IRFunction {
        std::string name;
        std::vector<std::unique_ptr<BasicBlock>> blocks;
        int vRegCount = 0;
        int localCount = 0;
        int argCount = 0;
        bool isInline = false;
        bool isExported = false;

        IRFunction() = default;
        IRFunction(const IRFunction&) = delete;
        IRFunction& operator=(const IRFunction&) = delete;
        IRFunction(IRFunction&&) noexcept = default;
        IRFunction& operator=(IRFunction&&) noexcept = default;

        int allocVReg() { return vRegCount++; }

        BasicBlock* createBlock(const std::string& label = "") {
            int id = blocks.size();
            blocks.push_back(std::make_unique<BasicBlock>());
            blocks.back()->id = id;
            blocks.back()->name = label.empty() ? ".L" + std::to_string(id) : label;
            return blocks.back().get();
        }

        void print(std::ostream& out) const {
            out << "define " << name << "() {\n";
            for (const auto& b : blocks) {
                out << b->name << ":\n";
                for (const auto& i : b->instructions) out << "  " << i.toString() << "\n";
            }
            out << "}\n\n";
        }
    };

    struct IRModule {
        std::vector<IRFunction> functions;
        std::vector<std::string> readOnlyStrings;

        IRModule() = default;
        IRModule(const IRModule&) = delete;
        IRModule& operator=(const IRModule&) = delete;
        IRModule(IRModule&&) noexcept = default;
        IRModule& operator=(IRModule&&) noexcept = default;

        void print(std::ostream& out) const { for (auto& f : functions) f.print(out); }
    };
}