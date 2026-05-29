#pragma once
#include "ir.hpp"
#include "emitter.hpp"
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <set>

namespace gbpp {

    enum class Target {
        Win64,
        SystemV,
    };

    struct FoldedAddr {
        int baseVReg = -1;
        int offset = 0;
    };

    class CodeGen {
    public:
        virtual ~CodeGen() = default;
        static std::unique_ptr<CodeGen> create(Target target);
        virtual void generate(IRModule& mod, Emitter& emitter) = 0;

    protected:
        std::string getReg(int id, int bytes);
        std::set<int> analyzeFunction(
            IRFunction& fn,
            std::map<int, Instruction*>& defs,
            std::map<int, std::vector<int>>& users,
            std::map<int, uint64_t>& constants,
            std::map<int, FoldedAddr>& foldedAddrs
        );
    };
}