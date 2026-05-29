#pragma once
#include "mir.hpp"
#include <vector>
#include <string>
#include <iostream>
#include <cstdint>
#include <memory>
#include <map>

namespace gbpp {

    struct Fixup {
        size_t offset;
        std::string symbol;
    };

    class BinaryBuffer {
    public:
        std::vector<uint8_t> data;
        std::vector<Fixup> fixups;

        void emit8(uint8_t v) { data.push_back(v); }
        void emit16(uint16_t v) { data.insert(data.end(), (uint8_t*)&v, (uint8_t*)&v + 2); }
        void emit32(uint32_t v) { data.insert(data.end(), (uint8_t*)&v, (uint8_t*)&v + 4); }
        void emit64(uint64_t v) { data.insert(data.end(), (uint8_t*)&v, (uint8_t*)&v + 8); }

        void addFixup(const std::string& sym) {
            fixups.push_back({ data.size(), sym });
            emit32(0);
        }

        void patch32(size_t offset, uint32_t v) {
            *(uint32_t*)(&data[offset]) = v;
        }
        size_t size() const { return data.size(); }
        const uint8_t* bytes() const { return data.data(); }
    };

    class Emitter {
    public:
        virtual ~Emitter() = default;

        virtual void enterTextSection() = 0;
        virtual void enterDataSection() = 0;

        virtual void emitGlobal(const std::string& name) = 0;
        virtual void emitExtern(const std::string& name) = 0;
        virtual void emitDataString(const std::string& label, const std::string& str) = 0;
        virtual void emitLabel(const std::string& label) = 0;

        virtual void emitInstruction(const MachineInstr& inst) = 0;
        virtual void finalize(std::ostream& out) = 0;

        static std::unique_ptr<Emitter> createAsm();
        static std::unique_ptr<Emitter> createCoffWin64();
        static std::unique_ptr<Emitter> createElfSysV();
    };
}