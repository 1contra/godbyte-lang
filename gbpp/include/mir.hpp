#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace gbpp {

    enum class MOperandType {
        Reg,
        Imm,
        Mem,
        Label
    };

    struct MachineOperand {
        MOperandType type;
        int size;

        union {
            int reg;
            uint64_t imm;
            struct {
                int baseReg;
                int offset;
            } mem;
        };
        std::string label;

        static MachineOperand createReg(int r, int size = 8) {
            MachineOperand op; op.type = MOperandType::Reg; op.size = size; op.reg = r; return op;
        }
        static MachineOperand createImm(uint64_t i, int size = 8) {
            MachineOperand op; op.type = MOperandType::Imm; op.size = size; op.imm = i; return op;
        }
        static MachineOperand createMem(int base, int off, int size = 8) {
            MachineOperand op; op.type = MOperandType::Mem; op.size = size;
            op.mem.baseReg = base; op.mem.offset = off; return op;
        }
        static MachineOperand createLabel(const std::string& l, int size = 8) {
            MachineOperand op; op.type = MOperandType::Label; op.size = size; op.label = l; return op;
        }

        bool isReg() const { return type == MOperandType::Reg; }
        bool isMem() const { return type == MOperandType::Mem; }
        bool isImm() const { return type == MOperandType::Imm; }
        bool isLabel() const { return type == MOperandType::Label; }
    };

    enum class MInstOpcode {
        X86_MOVrr, X86_MOVri, X86_MOVrm, X86_MOVmr, X86_MOVmi,
        X86_MOVZX,
        X86_ADDrr, X86_ADDri, X86_ADDrm, X86_ADDmr,
        X86_SUBrr, X86_SUBri, X86_SUBrm, X86_SUBmr,
        X86_IMULrr, X86_IMULrri,
        X86_IDIVr, X86_CQO,
        X86_ANDri, X86_ORrr, X86_ORri, X86_ORrm, X86_ORmr,
        X86_XORrr, X86_XORri, X86_XORrm, X86_XORmr,
        X86_SHLr, X86_SHRr, X86_SHLcl, X86_SHRcl,
        X86_TESTrr,
        X86_CMPrr, X86_CMPri, X86_CMPrm,

        X86_MOVSS, X86_MOVSD,
        X86_MOVAPS, X86_MOVUPS,
        X86_MOVDQA, X86_MOVDQU,
        X86_ADDSS, X86_ADDSD,
        X86_SUBSS, X86_SUBSD,
        X86_MULSS, X86_MULSD,
        X86_DIVSS, X86_DIVSD,
        X86_PADDD, X86_PSUBD,
        X86_PMULLD, X86_XORPS,
        X86_PSHUFD,
        X86_CVTTSD2SI, X86_CVTSI2SD,

        X86_LEAr, X86_LEAm,
        X86_CALLpcrel, X86_CALLr,
        X86_JMP, X86_JE, X86_JNE, X86_JL, X86_JLE, X86_JG, X86_JGE,
        X86_SETL, X86_SETG, X86_SETE, X86_SETNE, X86_SETGE, X86_SETLE,
        X86_PUSHr, X86_POPr,
        X86_LEAVE, X86_RET,
        X86_LABEL,
        X86_INLINE_ASM,

        X86_ANDrm, X86_ANDmr, X86_ANDrr
    };

    struct MachineInstr {
        MInstOpcode opcode;
        std::vector<MachineOperand> operands;
    };

    struct MIRBasicBlock {
        std::string name;
        std::vector<MachineInstr> insts;
    };

    struct MIRFunction {
        std::string name;
        std::vector<MIRBasicBlock> blocks;
        int frameSize = 0;
        std::vector<int> usedCalleeSaved;
    };

    struct MIRModule {
        std::vector<MIRFunction> functions;
    };
}