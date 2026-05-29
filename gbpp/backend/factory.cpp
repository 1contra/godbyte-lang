#include "../include/codegen.hpp"
#include <iostream>

namespace gbpp {
    std::unique_ptr<CodeGen> createSystemV();
    std::unique_ptr<CodeGen> createWin64();

    std::unique_ptr<CodeGen> CodeGen::create(Target target) {
        if (target == Target::SystemV) return createSystemV();
        return createWin64();
    }

    std::string CodeGen::getReg(int id, int bytes) {
        static const char* r64[] = { "rax", "rcx", "rdx", "rbx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15" };
        static const char* r32[] = { "eax", "ecx", "edx", "ebx", "esi", "edi", "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d" };
        static const char* r16[] = { "ax", "cx", "dx", "bx", "si", "di", "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w" };
        static const char* r8[] = { "al", "cl", "dl", "bl", "sil", "dil", "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b" };

        if (id < 0 || id > 13) return "err";
        if (bytes == 8) return r64[id];
        if (bytes == 4) return r32[id];
        if (bytes == 2) return r16[id];
        return r8[id];
    }

    std::set<int> CodeGen::analyzeFunction(
        IRFunction& fn,
        std::map<int, Instruction*>& defs,
        std::map<int, std::vector<int>>& users,
        std::map<int, uint64_t>& constants,
        std::map<int, FoldedAddr>& foldedAddrs
    ) {
        std::set<int> skippedInstIndices;
        std::map<int, int> defCounts;

        std::vector<Instruction*> flatInsts;
        for (const auto& block : fn.blocks) {
            for (auto& inst : block->instructions) {
                flatInsts.push_back(&inst);
            }
        }

        for (int i = 0; i < flatInsts.size(); ++i) {
            auto& inst = *flatInsts[i];
            defs[inst.dest] = &inst;
            if (inst.dest != -1) defCounts[inst.dest]++;
            if (inst.src1 != -1) users[inst.src1].push_back(i);
            if (inst.src2 != -1) users[inst.src2].push_back(i);
        }

        for (int i = 0; i < flatInsts.size(); ++i) {
            auto& inst = *flatInsts[i];
            if (inst.op == OpCode::CONST && defCounts[inst.dest] == 1) {
                constants[inst.dest] = inst.imm;
            }
        }

        for (int i = 0; i < flatInsts.size(); ++i) {
            auto& inst = *flatInsts[i];
            if (inst.op == OpCode::ADD) {
                if (inst.src2 == -1) {
                    foldedAddrs[inst.dest] = { inst.src1, (int)inst.imm };
                }
                else if (constants.count(inst.src2)) {
                    foldedAddrs[inst.dest] = { inst.src1, (int)constants[inst.src2] };
                }
                else if (constants.count(inst.src1)) {
                    foldedAddrs[inst.dest] = { inst.src2, (int)constants[inst.src1] };
                }
            }
        }

        for (int i = 0; i < flatInsts.size(); ++i) {
            auto& inst = *flatInsts[i];

            if (inst.op == OpCode::ADD && foldedAddrs.count(inst.dest)) {
                bool allUsesFoldable = true;
                for (int uIdx : users[inst.dest]) {
                    OpCode uOp = flatInsts[uIdx]->op;
                    if (uOp != OpCode::LOAD && uOp != OpCode::STORE) allUsesFoldable = false;
                }
                if (allUsesFoldable && !users[inst.dest].empty()) skippedInstIndices.insert(i);
            }

            if (inst.op == OpCode::CMP_LT || inst.op == OpCode::CMP_EQ || inst.op == OpCode::CMP_NE ||
                inst.op == OpCode::CMP_GT || inst.op == OpCode::CMP_GE || inst.op == OpCode::CMP_LE) {
                bool consumedByJmp = true;
                for (int uIdx : users[inst.dest]) {
                    if (flatInsts[uIdx]->op != OpCode::JMP_FALSE) consumedByJmp = false;
                }
                if (consumedByJmp && !users[inst.dest].empty()) skippedInstIndices.insert(i);
            }
        }

        for (int i = 0; i < flatInsts.size(); ++i) {
            if (flatInsts[i]->op == OpCode::CONST && defCounts[flatInsts[i]->dest] == 1) {
                int vReg = flatInsts[i]->dest;
                const auto& useList = users[vReg];
                bool allUsersSkipped = true;
                if (useList.empty()) {
                    allUsersSkipped = true;
                }
                else {
                    for (int uIdx : useList) {
                        if (skippedInstIndices.find(uIdx) == skippedInstIndices.end()) {
                            allUsersSkipped = false;
                            break;
                        }
                    }
                }
                if (allUsersSkipped) skippedInstIndices.insert(i);
            }
        }

        return skippedInstIndices;
    }
}