#pragma once
#include <vector>
#include <cstdint>
#include <cstring>

namespace gbpp {
    class BinaryBuffer {
        std::vector<uint8_t> buffer;
    public:
        void emit8(uint8_t val) { buffer.push_back(val); }

        void emit16(uint16_t val) {
            size_t curr = buffer.size();
            buffer.resize(curr + 2);
            std::memcpy(buffer.data() + curr, &val, 2);
        }

        void emit32(uint32_t val) {
            size_t curr = buffer.size();
            buffer.resize(curr + 4);
            std::memcpy(buffer.data() + curr, &val, 4);
        }

        void emit64(uint64_t val) {
            size_t curr = buffer.size();
            buffer.resize(curr + 8);
            std::memcpy(buffer.data() + curr, &val, 8);
        }

        void patch32(size_t offset, uint32_t val) {
            std::memcpy(buffer.data() + offset, &val, 4);
        }

        size_t size() const { return buffer.size(); }
        const uint8_t* data() const { return buffer.data(); }
    };
}