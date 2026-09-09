#include "../include/emitter.hpp"
#include "x86_encoder.hpp"
#include <sstream>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <fstream>
#include <map>
#include <cmath>

namespace gbpp {
    class DebugDumpEmitter : public Emitter {
        std::stringstream ss;
        uint64_t currentVa = 0x0000000000101000;

        std::map<std::string, std::vector<std::string>> fileCache;
        std::string lastFile = "";
        int lastLine = -1;

        std::string getSourceLine(const std::string& file, int line) {
            if (fileCache.find(file) == fileCache.end()) {
                std::ifstream ifs(file);
                std::vector<std::string> lines;
                std::string l;
                while (std::getline(ifs, l)) {
                    lines.push_back(l);
                }
                fileCache[file] = lines;
            }
            if (line > 0 && line <= fileCache[file].size()) {
                std::string src = fileCache[file][line - 1];
                src.erase(src.find_last_not_of(" \t\r\n") + 1);
                return src;
            }
            return "";
        }

        std::string getRegName(int id, int bytes) {
            if (bytes == 32) return "ymm" + std::to_string(id);
            if (bytes == 16) return "xmm" + std::to_string(id);
            static const char* r64[] = { "rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi","r8","r9","r10","r11","r12","r13","r14","r15" };
            static const char* r32[] = { "eax","ecx","edx","ebx","esp","ebp","esi","edi","r8d","r9d","r10d","r11d","r12d","r13d","r14d","r15d" };
            static const char* r16[] = { "ax","cx","dx","bx","sp","bp","si","di","r8w","r9w","r10w","r11w","r12w","r13w","r14w","r15w" };
            static const char* r8[] = { "al","cl","dl","bl","spl","bpl","sil","dil","r8b","r9b","r10b","r11b","r12b","r13b","r14b","r15b" };
            if (id >= 16 && id <= 31) return "xmm" + std::to_string(id - 16);
            if (bytes == 8) return r64[id];
            if (bytes == 4) return r32[id];
            if (bytes == 2) return r16[id];
            return r8[id];
        }

        std::string getSizeName(int bytes) {
            if (bytes == 32) return "ymmword ptr";
            if (bytes == 16) return "xmmword ptr";
            if (bytes == 8) return "qword ptr";
            if (bytes == 4) return "dword ptr";
            if (bytes == 2) return "word ptr";
            return "byte ptr";
        }

        std::string formatOperand(const MachineOperand& op) {
            if (op.type == MOperandType::Reg) return getRegName(op.reg, op.size);
            if (op.type == MOperandType::Imm) {
                std::stringstream is;
                if (op.imm > 0x1000) is << "0x" << std::hex << std::uppercase << op.imm;
                else is << op.imm;
                return is.str();
            }
            if (op.type == MOperandType::Label) return op.label;
            if (op.type == MOperandType::Mem) {
                std::string base = getRegName(op.mem.baseReg, 8);
                std::string off = op.mem.offset > 0 ? "+" + std::to_string(op.mem.offset) :
                    (op.mem.offset < 0 ? "-" + std::to_string(-op.mem.offset) : "");
                return getSizeName(op.size) + " [" + base + off + "]";
            }
            return "err";
        }

        std::string getMnemonic(MInstOpcode op) {
            switch (op) {
            case MInstOpcode::X86_MOVrr: case MInstOpcode::X86_MOVri: case MInstOpcode::X86_MOVrm: case MInstOpcode::X86_MOVmr: case MInstOpcode::X86_MOVmi: return "mov";
            case MInstOpcode::X86_MOVZX: return "movzx";
            case MInstOpcode::X86_MOVSX: return "movsx";
            case MInstOpcode::X86_MOVSXD: return "movsxd";
            case MInstOpcode::X86_ADDrr: case MInstOpcode::X86_ADDri: case MInstOpcode::X86_ADDrm: case MInstOpcode::X86_ADDmr: return "add";
            case MInstOpcode::X86_SUBrr: case MInstOpcode::X86_SUBri: case MInstOpcode::X86_SUBrm: case MInstOpcode::X86_SUBmr: return "sub";
            case MInstOpcode::X86_INC: return "inc";
            case MInstOpcode::X86_DEC: return "dec";
            case MInstOpcode::X86_IMULrr: case MInstOpcode::X86_IMULrri: return "imul";
            case MInstOpcode::X86_IDIVr: return "idiv";
            case MInstOpcode::X86_DIVr: return "div";
            case MInstOpcode::X86_CQO: return "cqo";
            case MInstOpcode::X86_ANDrr: case MInstOpcode::X86_ANDri: case MInstOpcode::X86_ANDrm: case MInstOpcode::X86_ANDmr: return "and";
            case MInstOpcode::X86_ORrr: case MInstOpcode::X86_ORri: case MInstOpcode::X86_ORrm: case MInstOpcode::X86_ORmr: return "or";
            case MInstOpcode::X86_XORrr: case MInstOpcode::X86_XORri: case MInstOpcode::X86_XORrm: case MInstOpcode::X86_XORmr: return "xor";
            case MInstOpcode::X86_SHLr: case MInstOpcode::X86_SHLcl: return "shl";
            case MInstOpcode::X86_SHRr: case MInstOpcode::X86_SHRcl: return "shr";
            case MInstOpcode::X86_TESTrr: return "test";
            case MInstOpcode::X86_CMPrr: case MInstOpcode::X86_CMPri: case MInstOpcode::X86_CMPrm: return "cmp";
            case MInstOpcode::X86_LEAr: case MInstOpcode::X86_LEAm: return "lea";
            case MInstOpcode::X86_CALLpcrel: case MInstOpcode::X86_CALLr: return "call";
            case MInstOpcode::X86_JMP: return "jmp";
            case MInstOpcode::X86_JE: return "je"; case MInstOpcode::X86_JNE: return "jne";
            case MInstOpcode::X86_JL: return "jl"; case MInstOpcode::X86_JLE: return "jle";
            case MInstOpcode::X86_JG: return "jg"; case MInstOpcode::X86_JGE: return "jge";
            case MInstOpcode::X86_SETL: return "setl"; case MInstOpcode::X86_SETG: return "setg";
            case MInstOpcode::X86_SETE: return "sete"; case MInstOpcode::X86_SETNE: return "setne";
            case MInstOpcode::X86_SETGE: return "setge"; case MInstOpcode::X86_SETLE: return "setle";
            case MInstOpcode::X86_PUSHr: return "push"; case MInstOpcode::X86_POPr: return "pop";
            case MInstOpcode::X86_LEAVE: return "leave"; case MInstOpcode::X86_RET: return "ret";
            case MInstOpcode::X86_CMOVE:  return "cmove";
            case MInstOpcode::X86_CMOVNE: return "cmovne";
            case MInstOpcode::X86_CMOVL:  return "cmovl";
            case MInstOpcode::X86_CMOVLE: return "cmovle";
            case MInstOpcode::X86_CMOVG:  return "cmovg";
            case MInstOpcode::X86_CMOVGE: return "cmovge";
            case MInstOpcode::X86_VPADDD: return "vpaddd";
            case MInstOpcode::X86_VPADDQ: return "vpaddq";
            case MInstOpcode::X86_VPSUBD: return "vpsubd";
            case MInstOpcode::X86_VPSUBQ: return "vpsubq";
            case MInstOpcode::X86_VPMULLD: return "vpmulld";
            case MInstOpcode::X86_VPMULUDQ: return "vpmuludq";
            case MInstOpcode::X86_VPAND: return "vpand";
            case MInstOpcode::X86_VPOR: return "vpor";
            case MInstOpcode::X86_VPXOR: return "vpxor";
            case MInstOpcode::X86_VMOVDQU: return "vmovdqu";
            case MInstOpcode::X86_VPBROADCASTD: return "vpbroadcastd";
            case MInstOpcode::X86_VPBROADCASTQ: return "vpbroadcastq";
            case MInstOpcode::X86_BSWAP: return "bswap";
            case MInstOpcode::X86_ROL8:  return "rol";
            case MInstOpcode::X86_INT3:  return "int3";
            case MInstOpcode::X86_UD2:   return "ud2";
            default: return "; unknown_op";
            }
        }

    public:
        DebugDumpEmitter() {
            ss << "GodByte++ Debug Dumper (Divo Build System)\n";
            ss << "Generated IR & Assembly Map\n\n";
            ss << "File Type: EXECUTABLE IMAGE (Debug Dump Map)\n\n";
        }

        void enterTextSection() override {}
        void enterDataSection() override {}
        void emitGlobal(const std::string&) override {}
        void emitExtern(const std::string&) override {}
        void emitDataString(const std::string&, const std::string&) override {}
        void emitDataInteger(const std::string&, uint64_t, int) override {}
        void emitLabel(const std::string&) override {}

        void emitInstruction(const MachineInstr& inst) override {
            if (inst.loc.line > 0 && !inst.loc.filename.empty()) {
                if (inst.loc.filename != lastFile || inst.loc.line < lastLine || inst.loc.line > lastLine + 3) {
                    lastFile = inst.loc.filename;
                    lastLine = inst.loc.line;

                    ss << "\n; " << std::string(80, '=') << "\n";
                    ss << "; SOURCE CONTEXT: " << lastFile << ":" << lastLine << "\n";
                    ss << "; " << std::string(80, '-') << "\n";

                    int startLine = std::max(1, lastLine - 5);
                    int endLine = lastLine + 1;

                    for (int l = startLine; l <= endLine; ++l) {
                        std::string src = getSourceLine(lastFile, l);
                        if (!src.empty() || l == lastLine) {
                            if (l == lastLine) {
                                ss << ";   >>  " << src << "\n";
                            }
                            else {
                                ss << ";       " << src << "\n";
                            }
                        }
                    }
                    ss << "; " << std::string(80, '=') << "\n";
                }

                else if (inst.loc.line > lastLine) {
                    for (int l = lastLine + 1; l <= inst.loc.line; ++l) {
                        std::string src = getSourceLine(lastFile, l);
                        if (l == inst.loc.line) {
                            ss << ";   >>  " << src << "\n";
                        }
                        else {
                            ss << ";       " << src << "\n";
                        }
                    }
                    lastLine = inst.loc.line;
                }
            }

            const size_t COMMENT_START_COL = 56;

            if (inst.opcode == MInstOpcode::X86_INLINE_ASM) {
                ss << "  " << std::hex << std::setw(16) << std::setfill('0') << std::uppercase << currentVa << ": ";
                ss << std::left << std::setw(20) << std::setfill(' ') << "";

                std::string inlineAsmStr = inst.operands[0].label;
                if (inlineAsmStr.length() < COMMENT_START_COL) {
                    inlineAsmStr.append(COMMENT_START_COL - inlineAsmStr.length(), ' ');
                }
                else {
                    inlineAsmStr += "  ";
                }

                ss << inlineAsmStr;
                if (!inst.ir_ref.empty()) ss << "// " << inst.ir_ref;
                ss << "\n";
                return;
            }

            BinaryBuffer tempBuf;
            X86Encoder::encode(inst, tempBuf);
            const uint8_t* bytes = tempBuf.bytes();
            size_t numBytes = tempBuf.size();

            std::string asmText = getMnemonic(inst.opcode);
            std::string opText = "";
            for (size_t i = 0; i < inst.operands.size(); ++i) {
                opText += formatOperand(inst.operands[i]);
                if (i < inst.operands.size() - 1) opText += ",";
            }

            size_t chunkIdx = 0;

            while (chunkIdx < numBytes || chunkIdx == 0) {
                if (chunkIdx == 0) {
                    ss << "  " << std::hex << std::setw(16) << std::setfill('0') << std::uppercase << currentVa << ": ";
                }
                else {
                    ss << "                    ";
                }

                std::string hexCol = "";
                size_t bytesToPrint = std::min((size_t)6, numBytes - chunkIdx);
                for (size_t i = 0; i < bytesToPrint; ++i) {
                    std::stringstream hexStream;
                    hexStream << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << (int)bytes[chunkIdx + i];
                    hexCol += hexStream.str() + " ";
                }

                ss << std::left << std::setw(19) << std::setfill(' ') << hexCol;

                if (chunkIdx == 0) {
                    std::string asmPart = asmText;
                    if (asmPart.length() < 12) asmPart.append(12 - asmPart.length(), ' ');

                    asmPart += opText;

                    if (asmPart.length() < COMMENT_START_COL) {
                        asmPart.append(COMMENT_START_COL - asmPart.length(), ' ');
                    }
                    else {
                        asmPart += "  ";
                    }

                    ss << asmPart;

                    if (!inst.ir_ref.empty()) {
                        ss << "// " << inst.ir_ref;
                    }
                }
                ss << "\n";
                chunkIdx += 6;
                if (chunkIdx >= numBytes) break;
            }
            currentVa += numBytes;
        }

        void finalize(std::ostream& out) override {
            out << ss.str();
        }
    };

    std::unique_ptr<Emitter> Emitter::createDebugDump() {
        return std::make_unique<DebugDumpEmitter>();
    }
}