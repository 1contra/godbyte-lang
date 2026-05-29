#include "x86_encoder.hpp"

namespace gbpp {

    void X86Encoder::emitRex(BinaryBuffer& buf, bool w, int r, int rm) {
        uint8_t rex = 0x40;
        if (w) rex |= 0x08;
        if (r >= 8) rex |= 0x04;
        if (rm >= 8) rex |= 0x01;
        if (rex != 0x40 || w) buf.emit8(rex);
    }

    void X86Encoder::emitModRM(BinaryBuffer& buf, int r, const MachineOperand& rm) {
        int reg = r & 7;
        if (rm.isReg()) {
            buf.emit8(0xC0 | (reg << 3) | (rm.reg & 7));
        }
        else if (rm.isMem()) {
            int base = rm.mem.baseReg & 7;
            int off = rm.mem.offset;
            bool needsSib = (base == 4);

            int mod = 0;
            if (off == 0 && base != 5) mod = 0;
            else if (off >= -128 && off <= 127) mod = 1;
            else mod = 2;

            buf.emit8((mod << 6) | (reg << 3) | base);
            if (needsSib) buf.emit8(0x24);

            if (mod == 1) buf.emit8(off & 0xFF);
            else if (mod == 2 || (mod == 0 && base == 5)) buf.emit32(off);
        }
    }

    void X86Encoder::emitBinaryGen(BinaryBuffer& buf, const MachineInstr& inst, uint8_t opMR, uint8_t opRM, uint8_t digitId) {
        auto& dst = inst.operands[0];
        auto& src = inst.operands[1];
        bool w = (dst.size == 8);

        if (dst.isReg() && src.isReg()) {
            emitRex(buf, w, src.reg, dst.reg);
            buf.emit8(dst.size == 1 ? (opMR - 1) : opRM);
            buf.emit8(0xC0 | ((src.reg & 7) << 3) | (dst.reg & 7));
        }
        else if (dst.isReg() && src.isMem()) {
            emitRex(buf, w, dst.reg, src.mem.baseReg);
            buf.emit8(dst.size == 1 ? (opRM - 1) : opRM);
            emitModRM(buf, dst.reg, src);
        }
        else if (dst.isMem() && src.isReg()) {
            emitRex(buf, w, src.reg, dst.mem.baseReg);
            buf.emit8(dst.size == 1 ? (opMR - 1) : opMR);
            emitModRM(buf, src.reg, dst);
        }
        else if (dst.isReg() && src.isImm()) {
            emitRex(buf, w, digitId, dst.reg);
            buf.emit8(dst.size == 1 ? 0x80 : 0x81);
            buf.emit8(0xC0 | (digitId << 3) | (dst.reg & 7));
            if (dst.size == 1) buf.emit8(src.imm);
            else buf.emit32(src.imm);
        }
        else if (dst.isMem() && src.isImm()) {
            emitRex(buf, w, digitId, dst.mem.baseReg);
            buf.emit8(dst.size == 1 ? 0x80 : 0x81);
            emitModRM(buf, digitId, dst);
            if (dst.size == 1) buf.emit8(src.imm);
            else buf.emit32(src.imm);
        }
    }

    void X86Encoder::encode(const MachineInstr& inst, BinaryBuffer& buf) {
        auto op = inst.opcode;
        if (op == MInstOpcode::X86_MOVrr || op == MInstOpcode::X86_MOVrm || op == MInstOpcode::X86_MOVmr || op == MInstOpcode::X86_MOVmi) {
            emitBinaryGen(buf, inst, 0x89, 0x8B, 0);
        }
        else if (op == MInstOpcode::X86_MOVri) {
            auto& dst = inst.operands[0]; auto& imm = inst.operands[1];
            bool w = (dst.size == 8);
            emitRex(buf, w, 0, dst.reg);
            if (dst.size == 8) {
                buf.emit8(0xB8 | (dst.reg & 7)); buf.emit64(imm.imm);
            }
            else {
                buf.emit8((dst.size == 1 ? 0xB0 : 0xB8) | (dst.reg & 7)); buf.emit32(imm.imm);
            }
        }
        else if (op == MInstOpcode::X86_MOVZX) {
            auto& dst = inst.operands[0]; auto& src = inst.operands[1];
            emitRex(buf, dst.size == 8, dst.reg, src.isMem() ? src.mem.baseReg : src.reg);
            buf.emit8(0x0F); buf.emit8(src.size == 1 ? 0xB6 : 0xB7);
            emitModRM(buf, dst.reg, src);
        }
        else if (op >= MInstOpcode::X86_ADDrr && op <= MInstOpcode::X86_ADDmr) emitBinaryGen(buf, inst, 0x01, 0x03, 0);
        else if (op >= MInstOpcode::X86_SUBrr && op <= MInstOpcode::X86_SUBmr) emitBinaryGen(buf, inst, 0x29, 0x2B, 5);
        else if (op >= MInstOpcode::X86_ANDrr && op <= MInstOpcode::X86_ANDmr) emitBinaryGen(buf, inst, 0x21, 0x23, 4);
        else if (op >= MInstOpcode::X86_ORrr && op <= MInstOpcode::X86_ORmr)  emitBinaryGen(buf, inst, 0x09, 0x0B, 1);
        else if (op >= MInstOpcode::X86_XORrr && op <= MInstOpcode::X86_XORmr) emitBinaryGen(buf, inst, 0x31, 0x33, 6);
        else if (op >= MInstOpcode::X86_CMPrr && op <= MInstOpcode::X86_CMPrm) emitBinaryGen(buf, inst, 0x39, 0x3B, 7);
        else if (op == MInstOpcode::X86_TESTrr) {
            auto& dst = inst.operands[0]; auto& src = inst.operands[1];
            emitRex(buf, dst.size == 8, src.reg, dst.reg);
            buf.emit8(0x85); buf.emit8(0xC0 | ((src.reg & 7) << 3) | (dst.reg & 7));
        }
        else if (op == MInstOpcode::X86_LEAr || op == MInstOpcode::X86_LEAm) {
            auto& dst = inst.operands[0]; auto& src = inst.operands[1];

            if (src.isLabel()) {
                emitRex(buf, dst.size == 8, dst.reg, 0);
                buf.emit8(0x8D);
                buf.emit8(0x05 | ((dst.reg & 7) << 3));
                buf.addFixup(src.label);
            }
            else {
                emitRex(buf, dst.size == 8, dst.reg, src.mem.baseReg);
                buf.emit8(0x8D);
                emitModRM(buf, dst.reg, src);
            }
        }
        else if (op == MInstOpcode::X86_CALLpcrel) {
            buf.emit8(0xE8);
            buf.addFixup(inst.operands[0].label);
        }
        else if (op == MInstOpcode::X86_CALLr) {
            emitRex(buf, false, 0, inst.operands[0].reg);
            buf.emit8(0xFF); buf.emit8(0xD0 | (inst.operands[0].reg & 7));
        }
        else if (op == MInstOpcode::X86_JMP) {
            buf.emit8(0xE9); buf.addFixup(inst.operands[0].label);
        }
        else if (op >= MInstOpcode::X86_JE && op <= MInstOpcode::X86_JGE) {
            int cond = 0;
            switch (op) {
                case MInstOpcode::X86_JE: cond = 0x84; break;
                case MInstOpcode::X86_JNE: cond = 0x85; break;
                case MInstOpcode::X86_JL: cond = 0x8C; break;
                case MInstOpcode::X86_JGE: cond = 0x8D; break;
                case MInstOpcode::X86_JLE: cond = 0x8E; break;
                case MInstOpcode::X86_JG: cond = 0x8F; break;
                default: break;
            }
            buf.emit8(0x0F); buf.emit8(cond); buf.addFixup(inst.operands[0].label);
        }
        else if (op >= MInstOpcode::X86_SETL && op <= MInstOpcode::X86_SETLE) {
            int cond = 0;
            switch (op) {
                case MInstOpcode::X86_SETL: cond = 0x9C; break;
                case MInstOpcode::X86_SETG: cond = 0x9F; break;
                case MInstOpcode::X86_SETE: cond = 0x94; break;
                case MInstOpcode::X86_SETNE: cond = 0x95; break;
                case MInstOpcode::X86_SETGE: cond = 0x9D; break;
                case MInstOpcode::X86_SETLE: cond = 0x9E; break;
                default: break;
            }
            emitRex(buf, false, 0, inst.operands[0].reg);
            buf.emit8(0x0F); buf.emit8(cond); buf.emit8(0xC0 | (inst.operands[0].reg & 7));
        }
        else if (op == MInstOpcode::X86_PUSHr) {
            emitRex(buf, false, 0, inst.operands[0].reg); buf.emit8(0x50 | (inst.operands[0].reg & 7));
        }
        else if (op == MInstOpcode::X86_POPr) {
            emitRex(buf, false, 0, inst.operands[0].reg); buf.emit8(0x58 | (inst.operands[0].reg & 7));
        }
        else if (op == MInstOpcode::X86_RET) { buf.emit8(0xC3); }
        else if (op == MInstOpcode::X86_LEAVE) { buf.emit8(0xC9); }
        else if (op == MInstOpcode::X86_CQO) { emitRex(buf, true, 0, 0); buf.emit8(0x99); }
    }
}