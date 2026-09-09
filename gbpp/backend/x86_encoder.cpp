#include "x86_encoder.hpp"

namespace gbpp {

    void X86Encoder::emitRex(BinaryBuffer& buf, bool w, int r, int rm, bool is8Bit) {
        uint8_t rex = 0x40;
        if (w) rex |= 0x08;
        if (r >= 8) rex |= 0x04;
        if (rm >= 8) rex |= 0x01;

        if (is8Bit && ((r >= 4 && r <= 7) || (rm >= 4 && rm <= 7))) {
            buf.emit8(rex);
        }
        else if (rex != 0x40 || w) {
            buf.emit8(rex);
        }
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

            if (off == 0 && base == 5) {
                mod = 1;
            }

            buf.emit8((mod << 6) | (reg << 3) | (needsSib ? 4 : base));

            if (needsSib) {
                buf.emit8(0x24);
            }

            if (mod == 1) {
                buf.emit8(off & 0xFF);
            }
            else if (mod == 2) {
                buf.emit32(off);
            }
        }
    }

    void X86Encoder::emitVEX(BinaryBuffer& buf, bool w, int r, int x, int b, int vvvv, int map_select, int pp, int L) {
        buf.emit8(0xC4);
        uint8_t byte1 = ((~r & 1) << 7) | ((~x & 1) << 6) | ((~b & 1) << 5) | (map_select & 0x1F);
        buf.emit8(byte1);
        uint8_t byte2 = ((w ? 1 : 0) << 7) | ((~vvvv & 0xF) << 3) | ((L & 1) << 2) | (pp & 0x3);
        buf.emit8(byte2);
    }

    void X86Encoder::emitBinaryGen(BinaryBuffer& buf, const MachineInstr& inst, uint8_t opMR, uint8_t opRM, uint8_t digitId) {
        auto& dst = inst.operands[0];
        auto& src = inst.operands[1];
        bool w = (dst.size == 8);
        bool is8Bit = (dst.size == 1);

        if (dst.isReg() && src.isReg()) {
            emitRex(buf, w, dst.reg, src.reg, is8Bit);
            buf.emit8(dst.size == 1 ? (opRM - 1) : opRM);
            buf.emit8(0xC0 | ((dst.reg & 7) << 3) | (src.reg & 7));
        }
        else if (dst.isReg() && src.isMem()) {
            emitRex(buf, w, dst.reg, src.mem.baseReg, is8Bit);
            buf.emit8(dst.size == 1 ? (opRM - 1) : opRM);
            emitModRM(buf, dst.reg, src);
        }
        else if (dst.isMem() && src.isReg()) {
            emitRex(buf, w, src.reg, dst.mem.baseReg, is8Bit);
            buf.emit8(dst.size == 1 ? (opMR - 1) : opMR);
            emitModRM(buf, src.reg, dst);
        }
        else if (dst.isReg() && src.isImm()) {
            emitRex(buf, w, digitId, dst.reg, is8Bit);
            if (opMR == 0x89) buf.emit8(dst.size == 1 ? 0xC6 : 0xC7);
            else buf.emit8(dst.size == 1 ? 0x80 : 0x81);

            buf.emit8(0xC0 | (digitId << 3) | (dst.reg & 7));
            if (dst.size == 1) buf.emit8(src.imm);
            else buf.emit32(src.imm);
        }
        else if (dst.isMem() && src.isImm()) {
            emitRex(buf, w, digitId, dst.mem.baseReg, is8Bit);
            if (opMR == 0x89) buf.emit8(dst.size == 1 ? 0xC6 : 0xC7);
            else buf.emit8(dst.size == 1 ? 0x80 : 0x81);

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
            auto& dst = inst.operands[0];
            auto& imm = inst.operands[1];

            bool w = (dst.size == 8);
            bool is8Bit = (dst.size == 1);

            if (dst.size == 2) {
                buf.emit8(0x66);
            }

            emitRex(buf, w, 0, dst.reg, is8Bit);

            if (dst.size == 1) {
                buf.emit8(0xB0 | (dst.reg & 7));
                buf.emit8(imm.imm);
            }
            else if (dst.size == 2) {
                buf.emit8(0xB8 | (dst.reg & 7));
                buf.emit16(imm.imm);
            }
            else if (dst.size == 4) {
                buf.emit8(0xB8 | (dst.reg & 7));
                buf.emit32(imm.imm);
            }
            else {
                buf.emit8(0xB8 | (dst.reg & 7));
                buf.emit64(imm.imm);
            }
        }
        else if (op == MInstOpcode::X86_MOVZX) {
            auto& dst = inst.operands[0];
            auto& src = inst.operands[1];

            if (dst.size == 2) {
                buf.emit8(0x66);
            }

            emitRex(buf, dst.size == 8, dst.reg, src.isMem() ? src.mem.baseReg : src.reg, src.size == 1);

            buf.emit8(0x0F);
            buf.emit8(src.size == 1 ? 0xB6 : 0xB7);
            emitModRM(buf, dst.reg, src);
        }
        else if (op >= MInstOpcode::X86_ADDrr && op <= MInstOpcode::X86_ADDmr) emitBinaryGen(buf, inst, 0x01, 0x03, 0);
        else if (op >= MInstOpcode::X86_SUBrr && op <= MInstOpcode::X86_SUBmr) emitBinaryGen(buf, inst, 0x29, 0x2B, 5);
        else if (op >= MInstOpcode::X86_ANDrr && op <= MInstOpcode::X86_ANDmr) emitBinaryGen(buf, inst, 0x21, 0x23, 4);
        else if (op >= MInstOpcode::X86_ORrr && op <= MInstOpcode::X86_ORmr)  emitBinaryGen(buf, inst, 0x09, 0x0B, 1);
        else if (op >= MInstOpcode::X86_XORrr && op <= MInstOpcode::X86_XORmr) emitBinaryGen(buf, inst, 0x31, 0x33, 6);
        else if (op >= MInstOpcode::X86_CMPrr && op <= MInstOpcode::X86_CMPrm) emitBinaryGen(buf, inst, 0x39, 0x3B, 7);
        else if (op == MInstOpcode::X86_INC || op == MInstOpcode::X86_DEC) {
            auto& dst = inst.operands[0];
            bool w = (dst.size == 8);

            uint8_t digitId = (op == MInstOpcode::X86_INC) ? 0 : 1;

            if (dst.isReg()) {
                emitRex(buf, w, 0, dst.reg);
                buf.emit8(dst.size == 1 ? 0xFE : 0xFF);
                buf.emit8(0xC0 | (digitId << 3) | (dst.reg & 7));
            }
            else if (dst.isMem()) {
                emitRex(buf, w, 0, dst.mem.baseReg);
                buf.emit8(dst.size == 1 ? 0xFE : 0xFF);
                emitModRM(buf, digitId, dst);
            }
        }
        else if (op == MInstOpcode::X86_SHLr || op == MInstOpcode::X86_SHRr ||
            op == MInstOpcode::X86_SHLcl || op == MInstOpcode::X86_SHRcl) {
            auto& dst = inst.operands[0];
            bool isLeft = (op == MInstOpcode::X86_SHLr || op == MInstOpcode::X86_SHLcl);
            bool isCl = (op == MInstOpcode::X86_SHLcl || op == MInstOpcode::X86_SHRcl);
            uint8_t digitId = isLeft ? 4 : 5;
            bool w = (dst.size == 8);
            bool is8Bit = (dst.size == 1);

            if (dst.size == 2) buf.emit8(0x66);

            emitRex(buf, w, 0, dst.isMem() ? dst.mem.baseReg : dst.reg, is8Bit);

            if (isCl) {
                buf.emit8(is8Bit ? 0xD2 : 0xD3);
                emitModRM(buf, digitId, dst);
            }
            else {
                auto& imm = inst.operands[1];
                if (imm.imm == 1) {
                    buf.emit8(is8Bit ? 0xD0 : 0xD1);
                    emitModRM(buf, digitId, dst);
                }
                else {
                    buf.emit8(is8Bit ? 0xC0 : 0xC1);
                    emitModRM(buf, digitId, dst);
                    buf.emit8(imm.imm & 0xFF);
                }
            }
        }
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
            emitRex(buf, false, 0, inst.operands[0].reg, true);
            buf.emit8(0x0F); buf.emit8(cond); buf.emit8(0xC0 | (inst.operands[0].reg & 7));
        }
        else if (op == MInstOpcode::X86_PUSHr) {
            emitRex(buf, false, 0, inst.operands[0].reg); buf.emit8(0x50 | (inst.operands[0].reg & 7));
        }
        else if (op == MInstOpcode::X86_POPr) {
            emitRex(buf, false, 0, inst.operands[0].reg); buf.emit8(0x58 | (inst.operands[0].reg & 7));
        }
        else if (op == MInstOpcode::X86_IDIVr || op == MInstOpcode::X86_DIVr) {
            auto& src = inst.operands[0];
            bool w = (src.size == 8);
            uint8_t digitId = (op == MInstOpcode::X86_IDIVr) ? 7 : 6;
            emitRex(buf, w, 0, src.isMem() ? src.mem.baseReg : src.reg, src.size == 1);
            buf.emit8(src.size == 1 ? 0xF6 : 0xF7);
            emitModRM(buf, digitId, src);
        }
        else if (op == MInstOpcode::X86_RET) { buf.emit8(0xC3); }
        else if (op == MInstOpcode::X86_LEAVE) { buf.emit8(0xC9); }
        else if (op == MInstOpcode::X86_CQO) { emitRex(buf, true, 0, 0); buf.emit8(0x99); }
        else if (op == MInstOpcode::X86_VPADDQ || op == MInstOpcode::X86_VPSUBQ
            || op == MInstOpcode::X86_VPMULUDQ || op == MInstOpcode::X86_VPAND
            || op == MInstOpcode::X86_VPOR || op == MInstOpcode::X86_VPXOR
        ) {
            auto& dst = inst.operands[0];
            auto& src1 = inst.operands[1];
            auto& src2 = inst.operands[2];
            int regDst = dst.reg & 7;
            int regSrc1 = src1.reg & 0xF;
            int regSrc2 = src2.reg & 7;
            int r = (dst.reg >> 3) & 1;
            int b = (src2.reg >> 3) & 1;

            int map_select = 2;
            uint8_t opc = 0;

            if (op == MInstOpcode::X86_VPADDQ) opc = 0xD4;
            else if (op == MInstOpcode::X86_VPSUBQ) opc = 0xFB;
            else if (op == MInstOpcode::X86_VPMULUDQ) { map_select = 1; opc = 0xF4; }
            else if (op == MInstOpcode::X86_VPAND) { map_select = 1; opc = 0xDB; }
            else if (op == MInstOpcode::X86_VPOR) { map_select = 1; opc = 0xEB; }
            else if (op == MInstOpcode::X86_VPXOR) { map_select = 1; opc = 0xEF; }

            emitVEX(buf, false, r, 0, b, regSrc1, map_select, 1, 1);
            buf.emit8(opc);
            buf.emit8(0xC0 | (regDst << 3) | regSrc2);
        }
        else if (op == MInstOpcode::X86_MOVDQU) {
            auto& dst = inst.operands[0];
            auto& src = inst.operands[1];
            if (dst.isReg() && src.isMem()) { // vmovdqu ymm, [mem]
                int r = (dst.reg >> 3) & 1;
                int b = (src.mem.baseReg >> 3) & 1;
                emitVEX(buf, false, r, 0, b, 0, 1, 2, 1); // map=1 (0F), pp=2 (F3)
                buf.emit8(0x6F);
                emitModRM(buf, dst.reg, src);
            }
            else if (dst.isMem() && src.isReg()) { // vmovdqu [mem], ymm
                int r = (src.reg >> 3) & 1;
                int b = (dst.mem.baseReg >> 3) & 1;
                emitVEX(buf, false, r, 0, b, 0, 1, 2, 1);
                buf.emit8(0x7F);
                emitModRM(buf, src.reg, dst);
            }
        }
        else if (op == MInstOpcode::X86_VPBROADCASTQ) {
            auto& dst = inst.operands[0];
            auto& src = inst.operands[1];

            int regDst = dst.reg & 7;
            int regSrc = src.reg & 7;

            // vmovq xmmDst, regSrc
            buf.emit8(0x66);
            emitRex(buf, true, dst.reg, src.reg);
            buf.emit8(0x0F);
            buf.emit8(0x6E);
            buf.emit8(0xC0 | (regDst << 3) | regSrc);

            // vpbroadcastq ymmDst, xmmDst
            int r = (dst.reg >> 3) & 1;
            int b = (dst.reg >> 3) & 1;

            emitVEX(buf, false, r, 0, b, 0, 2, 1, 1);
            buf.emit8(0x59);
            buf.emit8(0xC0 | (regDst << 3) | regDst);
        }
        else if (op == MInstOpcode::X86_VZEROUPPER) {
            buf.emit8(0xC5); buf.emit8(0xF8); buf.emit8(0x77);
        }
        else if (op >= MInstOpcode::X86_CMOVE && op <= MInstOpcode::X86_CMOVGE) {
            auto& dst = inst.operands[0];
            auto& src = inst.operands[1];

            int condCode = 0;
            switch (op) {
                case MInstOpcode::X86_CMOVE:  condCode = 0x44; break;
                case MInstOpcode::X86_CMOVNE: condCode = 0x45; break;
                case MInstOpcode::X86_CMOVL:  condCode = 0x4C; break;
                case MInstOpcode::X86_CMOVGE: condCode = 0x4D; break;
                case MInstOpcode::X86_CMOVLE: condCode = 0x4E; break;
                case MInstOpcode::X86_CMOVG:  condCode = 0x4F; break;
                default: break;
            }

            emitRex(buf, dst.size == 8, dst.reg, src.isMem() ? src.mem.baseReg : src.reg);
            buf.emit8(0x0F);
            buf.emit8(condCode);
            emitModRM(buf, dst.reg, src);
        }
        else if (op == MInstOpcode::X86_INT3) {
            buf.emit8(0xCC);
        }
        else if (op == MInstOpcode::X86_UD2) {
            buf.emit8(0x0F);
            buf.emit8(0x0B);
        }
        else if (op == MInstOpcode::X86_BSWAP) {
            auto& dst = inst.operands[0];
            emitRex(buf, dst.size == 8, 0, dst.reg);
            buf.emit8(0x0F);
            buf.emit8(0xC8 | (dst.reg & 7));
        }
        else if (op == MInstOpcode::X86_ROL8) {
            auto& dst = inst.operands[0];
            emitRex(buf, false, 0, dst.reg, true);
            buf.emit8(0xC0);
            buf.emit8(0xC0 | (dst.reg & 7));
            buf.emit8(0x08);
        }
        else if (op == MInstOpcode::X86_MOVSX) {
            auto& dst = inst.operands[0];
            auto& src = inst.operands[1];

            if (dst.size == 2) {
                buf.emit8(0x66);
            }

            emitRex(buf, dst.size == 8, dst.reg, src.isMem() ? src.mem.baseReg : src.reg, src.size == 1);

            buf.emit8(0x0F);
            buf.emit8(src.size == 1 ? 0xBE : 0xBF);
            emitModRM(buf, dst.reg, src);
        }
        else if (op == MInstOpcode::X86_MOVSXD) {
            auto& dst = inst.operands[0]; auto& src = inst.operands[1];
            emitRex(buf, true, dst.reg, src.isMem() ? src.mem.baseReg : src.reg);
            buf.emit8(0x63);
            emitModRM(buf, dst.reg, src);
        }
    }
}