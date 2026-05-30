#include "../include/emitter.hpp"
#include <sstream>

namespace gbpp {
    class AsmEmitter : public Emitter {
        std::stringstream ss;

        std::string getRegName(int id, int bytes) {
            static const char* r64[] = { "rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi","r8","r9","r10","r11","r12","r13","r14","r15" };
            static const char* r32[] = { "eax","ecx","edx","ebx","esp","ebp","esi","edi","r8d","r9d","r10d","r11d","r12d","r13d","r14d","r15d" };
            static const char* r16[] = { "ax","cx","dx","bx","sp","bp","si","di","r8w","r9w","r10w","r11w","r12w","r13w","r14w","r15w" };
            static const char* r8[] = { "al","cl","dl","bl","spl","bpl","sil","dil","r8b","r9b","r10b","r11b","r12b","r13b","r14b","r15b" };

            if (bytes == 8) return r64[id];
            if (bytes == 4) return r32[id];
            if (bytes == 2) return r16[id];
            return r8[id];
        }

        std::string getSizeName(int bytes) {
            if (bytes == 8) return "qword";
            if (bytes == 4) return "dword";
            if (bytes == 2) return "word";
            return "byte";
        }

        std::string formatOperand(const MachineOperand& op) {
            if (op.type == MOperandType::Reg) return getRegName(op.reg, op.size);
            if (op.type == MOperandType::Imm) return std::to_string(op.imm);
            if (op.type == MOperandType::Label) return op.label;
            if (op.type == MOperandType::Mem) {
                std::string base = getRegName(op.mem.baseReg, 8);
                std::string off = op.mem.offset > 0 ? " + " + std::to_string(op.mem.offset) :
                    (op.mem.offset < 0 ? " - " + std::to_string(-op.mem.offset) : "");
                return getSizeName(op.size) + " [" + base + off + "]";
            }
            return "err";
        }

        std::string getMnemonic(MInstOpcode op) {
            switch (op) {
            case MInstOpcode::X86_MOVrr: case MInstOpcode::X86_MOVri: case MInstOpcode::X86_MOVrm: case MInstOpcode::X86_MOVmr: case MInstOpcode::X86_MOVmi: return "mov";
            case MInstOpcode::X86_MOVZX: return "movzx";
            case MInstOpcode::X86_ADDrr: case MInstOpcode::X86_ADDri: case MInstOpcode::X86_ADDrm: case MInstOpcode::X86_ADDmr: return "add";
            case MInstOpcode::X86_SUBrr: case MInstOpcode::X86_SUBri: case MInstOpcode::X86_SUBrm: case MInstOpcode::X86_SUBmr: return "sub";
            case MInstOpcode::X86_IMULrr: case MInstOpcode::X86_IMULrri: return "imul";
            case MInstOpcode::X86_IDIVr: return "idiv";
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
            default: return "; unknown_op";
            }
        }

    public:
        AsmEmitter() { ss << "default rel\n\n"; }
        void enterTextSection() override { ss << "section .text\n"; }
        void enterDataSection() override { ss << "section .data\n"; }
        void emitGlobal(const std::string& name) override { ss << "global " << name << "\n"; }
        void emitExtern(const std::string& name) override { ss << "extern " << name << "\n"; }
        void emitDataString(const std::string& label, const std::string& str) override {
            ss << "    " << label << " db \"" << str << "\", 0\n";
        }
        void emitLabel(const std::string& label) override { ss << label << ":\n"; }

        void emitInstruction(const MachineInstr& inst) override {
            if (inst.opcode == MInstOpcode::X86_INLINE_ASM) {
                ss << "    " << inst.operands[0].label << "\n";
                return;
            }
            ss << "    " << getMnemonic(inst.opcode);
            for (size_t i = 0; i < inst.operands.size(); ++i) {
                std::string opStr = formatOperand(inst.operands[i]);
                if ((inst.opcode == MInstOpcode::X86_LEAr || inst.opcode == MInstOpcode::X86_LEAm) && inst.operands[i].type == MOperandType::Mem) {
                    size_t bracket = opStr.find('[');
                    if (bracket != std::string::npos) opStr = opStr.substr(bracket);
                }
                ss << (i == 0 ? " " : ", ") << opStr;
            }
            ss << "\n";
        }

        void finalize(std::ostream& out) override {
            out << ss.str();
        }
    };

    std::unique_ptr<Emitter> Emitter::createAsm() {
        return std::make_unique<AsmEmitter>();
    }
}