#pragma once
#include "../include/emitter.hpp"
#include "../include/mir.hpp"

namespace gbpp {
    class X86Encoder {
    public:
        static void encode(const MachineInstr& inst, BinaryBuffer& buf);
    private:
        static void emitRex(BinaryBuffer& buf, bool w, int r, int rm);
        static void emitModRM(BinaryBuffer& buf, int r, const MachineOperand& rm);
        static void emitBinaryGen(BinaryBuffer& buf, const MachineInstr& inst, uint8_t opMR, uint8_t opRM, uint8_t digitId);
    };
}