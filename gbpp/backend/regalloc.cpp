#include "../include/regalloc.hpp"
#include <algorithm>

namespace gbpp {
    AllocResult RegAlloc::allocate(IRFunction& fn, const TargetRegisterInfo& tri) {
        AllocResult result;
        result.spillSize = 0;

        std::vector<int> callIndices;
        std::map<int, Lifetime> lifetimes;
        std::map<int, int> blockStartIdx;
        std::map<int, int> definitions;
        std::map<int, int> regHints;

        int globalIdx = 0;
        for (const auto& block : fn.blocks) {
            blockStartIdx[block->id] = globalIdx;
            for (const auto& inst : block->instructions) {

                if (inst.op == OpCode::GET_PARAM) {
                    if (inst.imm < static_cast<int>(tri.argRegs.size()) && !regHints.count(inst.dest)) {
                        regHints[inst.dest] = tri.argRegs[inst.imm];
                    }
                }

                if (inst.op == OpCode::CALL) {
                    callIndices.push_back(globalIdx);
                    for (size_t i = 0; i < std::min(tri.argRegs.size(), inst.args.size()); ++i) {
                        int vArg = inst.args[i];
                        if (vArg != -1 && !regHints.count(vArg)) {
                            regHints[vArg] = tri.argRegs[i];
                        }
                    }
                }

                if (inst.op == OpCode::RET && inst.src1 != -1) {
                    if (!regHints.count(inst.src1)) regHints[inst.src1] = tri.returnReg;
                }

                if (inst.dest != -1 && !definitions.count(inst.dest)) definitions[inst.dest] = globalIdx;

                auto touch = [&](int v) {
                    if (v == -1 || v <= -100) return;
                    if (!lifetimes.count(v)) lifetimes[v] = { globalIdx, globalIdx };
                    lifetimes[v].end = std::max(lifetimes[v].end, globalIdx);
                    };

                touch(inst.dest);
                touch(inst.src1);
                touch(inst.src2);
                for (int arg : inst.args) touch(arg);

                globalIdx++;
            }
        }

        bool changed = true;
        while (changed) {
            changed = false;
            globalIdx = 0;
            for (const auto& block : fn.blocks) {
                for (const auto& inst : block->instructions) {
                    if (inst.op == OpCode::JMP || inst.op == OpCode::JMP_FALSE) {
                        int targetIdx = blockStartIdx[inst.imm];
                        if (targetIdx < globalIdx) {
                            for (auto& [v, life] : lifetimes) {
                                if (life.start <= targetIdx && life.end >= targetIdx) {
                                    if (life.end < globalIdx) {
                                        life.end = globalIdx;
                                        changed = true;
                                    }
                                }
                            }
                        }
                    }
                    globalIdx++;
                }
            }
        }

        std::vector<int> vRegs;
        for (auto const& [vreg, life] : lifetimes) vRegs.push_back(vreg);
        std::sort(vRegs.begin(), vRegs.end(), [&](int a, int b) { return lifetimes[a].start < lifetimes[b].start; });
        const std::vector<int>& callerSaved = tri.callerSaved;
        const std::vector<int>& calleeSaved = tri.calleeSaved;

        std::map<int, int> active;

        for (int vReg : vRegs) {
            int start = lifetimes[vReg].start;
            int end = lifetimes[vReg].end;

            for (auto it = active.begin(); it != active.end(); ) {
                if (it->second < start) it = active.erase(it);
                else ++it;
            }

            bool crossesCall = false;
            std::vector<int> crossedCalls;
            for (int callIdx : callIndices) {
                if (start < callIdx && end > callIdx) {
                    crossesCall = true;
                    crossedCalls.push_back(callIdx);
                }
            }

            int picked = -1;

            if (crossesCall) {
                for (int reg : calleeSaved) {
                    if (active.find(reg) == active.end()) { picked = reg; break; }
                }

                if (picked == -1) {
                    for (int reg : callerSaved) {
                        if (active.find(reg) == active.end()) { picked = reg; break; }
                    }
                }
            }
            else {
                if (regHints.count(vReg)) {
                    int hint = regHints[vReg];
                    if (active.find(hint) == active.end()) { picked = hint; }
                }

                if (picked == -1) {
                    for (int reg : callerSaved) {
                        if (active.find(reg) == active.end()) { picked = reg; break; }
                    }
                }

                if (picked == -1) {
                    for (int reg : calleeSaved) {
                        if (active.find(reg) == active.end()) { picked = reg; break; }
                    }
                }
            }

            if (picked != -1) {
                result.registers[vReg] = picked;
                active[picked] = end;

                if (crossesCall && std::find(callerSaved.begin(), callerSaved.end(), picked) != callerSaved.end()) {
                    for (int cIdx : crossedCalls) {
                        result.callSpills[cIdx].push_back(vReg);
                    }
                    if (result.spills.find(vReg) == result.spills.end()) {
                        result.spillSize += 8;
                        result.spills[vReg] = result.spillSize;
                    }
                }
            }
            else {
                if (result.spills.find(vReg) == result.spills.end()) {
                    result.spillSize += 8;
                    result.spills[vReg] = result.spillSize;
                }
            }
        }
        return result;
    }
}