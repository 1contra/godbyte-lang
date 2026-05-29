#pragma once
#include "ir.hpp"
#include <map>
#include <string>
#include <vector>

namespace gbpp {

    struct Lifetime {
        int start = -1;
        int end = -1;
    };

    struct AllocResult {
        std::map<int, int> registers;
        std::map<int, int> spills;
        int spillSize = 0;
        std::map<int, std::vector<int>> callSpills;
    };

    struct TargetRegisterInfo {
        std::vector<int> argRegs;
        std::vector<int> callerSaved;
        std::vector<int> calleeSaved;
        int returnReg = -1;
    };

    class RegAlloc {
    public:
        AllocResult allocate(IRFunction& fn, const TargetRegisterInfo& tri);

    private:
        void analyzeLifetimes(const IRFunction& fn, std::map<int, Lifetime>& lifetimes);
    };

}