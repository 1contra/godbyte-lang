#include "../include/codegen.hpp"
#include "../include/regalloc.hpp"
#include "../include/mir.hpp"
#include "../include/emitter.hpp"
#include <map>
#include <vector>
#include <iostream>
#include <sstream>

namespace gbpp {

    void removeEmptyBlocks(MIRFunction& mirFn) {
        if (mirFn.blocks.empty()) return;

        std::map<std::string, std::string> aliases;
        std::set<std::string> toRemove;

        for (size_t i = 0; i < mirFn.blocks.size(); ++i) {
            auto& block = mirFn.blocks[i];

            if (block.insts.size() == 1 && block.insts[0].opcode == MInstOpcode::X86_JMP) {
                aliases[block.name] = block.insts[0].operands[0].label;
                toRemove.insert(block.name);
            }

            else if (block.insts.empty()) {
                if (i + 1 < mirFn.blocks.size()) {
                    aliases[block.name] = mirFn.blocks[i + 1].name;
                    toRemove.insert(block.name);
                }
            }
        }

        for (auto& [name, target] : aliases) {
            std::string finalTarget = target;
            std::set<std::string> visited = { name };
            while (aliases.count(finalTarget)) {
                if (visited.count(finalTarget)) break;
                visited.insert(finalTarget);
                finalTarget = aliases[finalTarget];
            }
            target = finalTarget;
        }

        for (auto& block : mirFn.blocks) {
            for (auto& inst : block.insts) {
                if (inst.opcode == MInstOpcode::X86_JMP ||
                    (inst.opcode >= MInstOpcode::X86_JE && inst.opcode <= MInstOpcode::X86_JGE)) {

                    std::string target = inst.operands[0].label;
                    if (aliases.count(target)) {
                        inst.operands[0].label = aliases[target];
                    }
                }
            }
        }

        mirFn.blocks.erase(std::remove_if(mirFn.blocks.begin(), mirFn.blocks.end(), [&](const MIRBasicBlock& b) {
            return toRemove.count(b.name);
        }), mirFn.blocks.end());
    }

    void removeUnreachableBlocks(MIRFunction& mirFn) {
        if (mirFn.blocks.empty()) return;

        std::set<std::string> reachable;
        std::vector<std::string> worklist;

        reachable.insert(mirFn.blocks[0].name);
        worklist.push_back(mirFn.blocks[0].name);

        auto getBlock = [&](const std::string& name) -> const MIRBasicBlock* {
            for (const auto& b : mirFn.blocks) if (b.name == name) return &b;
            return nullptr;
        };

        while (!worklist.empty()) {
            std::string curr = worklist.back();
            worklist.pop_back();

            const MIRBasicBlock* b = getBlock(curr);
            if (!b) continue;

            bool fallsThrough = true;

            for (const auto& inst : b->insts) {
                if (inst.opcode == MInstOpcode::X86_JMP) {
                    std::string target = inst.operands[0].label;
                    if (reachable.insert(target).second) worklist.push_back(target);
                    fallsThrough = false;
                    break;
                }
                else if (inst.opcode >= MInstOpcode::X86_JE && inst.opcode <= MInstOpcode::X86_JGE) {
                    std::string target = inst.operands[0].label;
                    if (reachable.insert(target).second) worklist.push_back(target);
                }
            }

            if (fallsThrough) {
                auto it = std::find_if(mirFn.blocks.begin(), mirFn.blocks.end(), [&](const MIRBasicBlock& mb) { return mb.name == curr; });
                if (it != mirFn.blocks.end() && std::next(it) != mirFn.blocks.end()) {
                    std::string nextName = std::next(it)->name;
                    if (reachable.insert(nextName).second) worklist.push_back(nextName);
                }
            }
        }

        mirFn.blocks.erase(std::remove_if(mirFn.blocks.begin(), mirFn.blocks.end(), [&](const MIRBasicBlock& b) {
            return reachable.find(b.name) == reachable.end();
        }), mirFn.blocks.end());
    }

    const int REG_RAX = 0; const int REG_RCX = 1; const int REG_RDX = 2; const int REG_RBX = 3;
    const int REG_RSP = 4; const int REG_RBP = 5; const int REG_RSI = 6; const int REG_RDI = 7;
    const int REG_R8 = 8; const int REG_R9 = 9; const int REG_R10 = 10; const int REG_R11 = 11;
    const int REG_R12 = 12; const int REG_R13 = 13; const int REG_R14 = 14; const int REG_R15 = 15;

    class CodeGenWin64 : public CodeGen {
        std::set<std::string> m_definedFunctions;

    public:
        void generate(IRModule& mod, Emitter& emitter) override {
            m_definedFunctions.clear();
            for (const auto& fn : mod.functions) {
                if (!fn.blocks.empty()) {
                    m_definedFunctions.insert(fn.name);
                }
            }

            std::set<std::string> externalCalls;
            for (const auto& fn : mod.functions) {
                for (const auto& block : fn.blocks) {
                    for (const auto& inst : block->instructions) {
                        if (inst.op == OpCode::CALL && !inst.label.empty()) {
                            if (!m_definedFunctions.count(inst.label)) {
                                externalCalls.insert(inst.label);
                            }
                        }
                    }
                }
            }

            for (const auto& fn : mod.functions) {
                if (fn.blocks.empty()) externalCalls.insert(fn.name);
            }

            if (!externalCalls.empty()) {
                for (const auto& ext : externalCalls) {
                    emitter.emitExtern(sanitizeLabel(ext));
                }
            }

            if (!mod.readOnlyStrings.empty()) {
                emitter.enterDataSection();
                for (size_t i = 0; i < mod.readOnlyStrings.size(); ++i) {
                    emitter.emitDataString("str_" + std::to_string(i), mod.readOnlyStrings[i]);
                }
            }

            emitter.enterTextSection();
            for (const auto& fn : mod.functions) {
                if (!fn.blocks.empty()) {
                    emitter.emitGlobal(sanitizeLabel(fn.name));
                }
            }

            for (auto& fn : mod.functions) genFunction(fn, emitter);
        }

    private:
        const std::vector<int> argRegs = { REG_RCX, REG_RDX, REG_R8, REG_R9 };

        struct AsmSequence {
            std::vector<MachineInstr> insts;
            int cost;
        };

        int estimateCost(MInstOpcode op, const MachineOperand& op1, const MachineOperand& op2) {
            int cost = 0;

            bool needsRex = (op1.size == 8 || (op1.isReg() && op1.reg >= 8) || (op2.isReg() && op2.reg >= 8));
            if (needsRex) cost += 1;

            switch (op) {
            case MInstOpcode::X86_XORrr:
            case MInstOpcode::X86_SUBrr:
            case MInstOpcode::X86_TESTrr:
                return cost + 2;

            case MInstOpcode::X86_MOVrr:
                return cost + 2;

            case MInstOpcode::X86_MOVri:
                return cost + 1 + (op1.size == 8 ? 8 : 4);

            case MInstOpcode::X86_ADDri:
            case MInstOpcode::X86_SUBri:
                if (op2.imm <= 127) return cost + 3;
                return cost + 1 + 4;

            default:
                return cost + 3;
            }
        }

        void selectOptimalLoadImm(std::vector<MachineInstr>& lir, MachineOperand dst, uint64_t imm) {
            std::vector<AsmSequence> candidates;

            MachineOperand immOp = MachineOperand::createImm(imm, dst.size);
            int movCost = estimateCost(MInstOpcode::X86_MOVri, dst, immOp);
            candidates.push_back({ {{ MInstOpcode::X86_MOVri, {dst, immOp} }}, movCost });

            if (imm == 0) {
                MachineOperand optDst = MachineOperand::createReg(dst.reg, 4);
                int xorCost = estimateCost(MInstOpcode::X86_XORrr, optDst, optDst);
                candidates.push_back({ {{ MInstOpcode::X86_XORrr, {optDst, optDst} }}, xorCost });
            }

            if (imm == 0) {
                MachineOperand optDst = MachineOperand::createReg(dst.reg, std::min(4, dst.size));
                int subCost = estimateCost(MInstOpcode::X86_SUBrr, optDst, optDst);
                candidates.push_back({ {{ MInstOpcode::X86_SUBrr, {optDst, optDst} }}, subCost });
            }

            auto best = std::min_element(candidates.begin(), candidates.end(),
                [](const AsmSequence& a, const AsmSequence& b) {
                    return a.cost < b.cost;
                });

            for (const auto& inst : best->insts) {
                lir.push_back(inst);
            }
        }

        std::string getRegName(int id, int bytes) {
            static const char* r64[] = { "rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi","r8","r9","r10","r11","r12","r13","r14","r15" };
            static const char* r32[] = { "eax","ecx","edx","ebx","esp","ebp","esi","edi","r8d","r9d","r10d","r11d","r12d","r13d","r14d","r15d" };
            static const char* r16[] = { "ax","cx","dx","bx","sp","bp","si","di","r8w","r9w","r10w","r11w","r12w","r13w","r14w","r15w" };
            static const char* r8[] = { "al","cl","dl","bl","spl","bpl","sil","dil","r8b","r9b","r10b","r11b","r12b","r13b","r14b","r15b" };

            if (id >= 16 && id <= 31) {
                return "xmm" + std::to_string(id - 16);
            }

            if (bytes == 8) return r64[id];
            if (bytes == 4) return r32[id];
            if (bytes == 2) return r16[id];
            return r8[id];
        }

        std::string getSizeName(int bytes) {
            if (bytes == 16) return "xmmword";
            if (bytes == 8) return "qword";
            if (bytes == 4) return "dword";
            if (bytes == 2) return "word";
            return "byte";
        }

        std::string formatImm(uint64_t imm, int size) {
            uint64_t masked = imm;
            if (size == 1) masked = imm & 0xFF;
            else if (size == 2) masked = imm & 0xFFFF;
            else if (size == 4) masked = imm & 0xFFFFFFFF;

            if (masked >= 0x1000) {
                std::stringstream ss;
                ss << "0x" << std::hex << std::uppercase << masked;
                return ss.str();
            }
            return std::to_string(masked);
        }

        std::string sanitizeLabel(std::string label) {
            size_t pos = 0;
            while ((pos = label.find("::", pos)) != std::string::npos) {
                label.replace(pos, 2, "__");
                pos += 2;
            }
            return label;
        }

        std::string formatOperand(const MachineOperand& op) {
            if (op.type == MOperandType::Reg) {
                if (op.reg == REG_RBP) return (op.size == 8) ? "rbp" : "ebp";
                if (op.reg == REG_RSP) return (op.size == 8) ? "rsp" : "esp";
                return getRegName(op.reg, op.size);
            }
            if (op.type == MOperandType::Imm) {
                return formatImm(op.imm, op.size);
            }
            if (op.type == MOperandType::Mem) {
                std::string base;
                if (op.mem.baseReg == REG_RBP) base = "rbp";
                else if (op.mem.baseReg == REG_RSP) base = "rsp";
                else base = getRegName(op.mem.baseReg, 8);

                std::string offStr = "";
                if (op.mem.offset > 0) offStr = " + " + std::to_string(op.mem.offset);
                else if (op.mem.offset < 0) offStr = " - " + std::to_string(-op.mem.offset);

                return getSizeName(op.size) + " [" + base + offStr + "]";
            }
            if (op.type == MOperandType::Label) {
                return sanitizeLabel(op.label);
            }
            return "err";
        }

        std::string getMnemonic(MInstOpcode op) {
            switch (op) {
                case MInstOpcode::X86_MOVrr: case MInstOpcode::X86_MOVri:
                case MInstOpcode::X86_MOVrm: case MInstOpcode::X86_MOVmr:
                case MInstOpcode::X86_MOVmi: return "mov";
                case MInstOpcode::X86_MOVZX: return "movzx";
                case MInstOpcode::X86_ADDrr: case MInstOpcode::X86_ADDri:
                case MInstOpcode::X86_ADDrm: case MInstOpcode::X86_ADDmr: return "add";
                case MInstOpcode::X86_SUBrr: case MInstOpcode::X86_SUBri:
                case MInstOpcode::X86_SUBrm: case MInstOpcode::X86_SUBmr: return "sub";
                case MInstOpcode::X86_IMULrr: case MInstOpcode::X86_IMULrri: return "imul";
                case MInstOpcode::X86_IDIVr: return "idiv";
                case MInstOpcode::X86_CQO: return "cqo";
                case MInstOpcode::X86_ANDri: return "and";
                case MInstOpcode::X86_ORrr: case MInstOpcode::X86_ORri:
                case MInstOpcode::X86_ORrm: case MInstOpcode::X86_ORmr: return "or";
                case MInstOpcode::X86_XORrr: return "xor";
                case MInstOpcode::X86_SHLr: case MInstOpcode::X86_SHLcl: return "shl";
                case MInstOpcode::X86_SHRr: case MInstOpcode::X86_SHRcl: return "shr";
                case MInstOpcode::X86_TESTrr: return "test";
                case MInstOpcode::X86_CMPrr: case MInstOpcode::X86_CMPri:
                case MInstOpcode::X86_CMPrm: return "cmp";
                case MInstOpcode::X86_LEAr: case MInstOpcode::X86_LEAm: return "lea";
                case MInstOpcode::X86_CALLpcrel: case MInstOpcode::X86_CALLr: return "call";
                case MInstOpcode::X86_JMP: return "jmp";
                case MInstOpcode::X86_JE: return "je";
                case MInstOpcode::X86_JNE: return "jne";
                case MInstOpcode::X86_JL: return "jl";
                case MInstOpcode::X86_JLE: return "jle";
                case MInstOpcode::X86_JG: return "jg";
                case MInstOpcode::X86_JGE: return "jge";
                case MInstOpcode::X86_PUSHr: return "push";
                case MInstOpcode::X86_POPr: return "pop";
                case MInstOpcode::X86_LEAVE: return "leave";
                case MInstOpcode::X86_RET: return "ret";
                case MInstOpcode::X86_MOVAPS: return "movaps";
                case MInstOpcode::X86_MOVUPS: return "movups";
                case MInstOpcode::X86_MOVDQA: return "movdqa";
                case MInstOpcode::X86_MOVDQU: return "movdqu";
                case MInstOpcode::X86_PADDD: return "paddd";
                case MInstOpcode::X86_PSUBD: return "psubd";
                case MInstOpcode::X86_XORPS: return "xorps";
                case MInstOpcode::X86_ADDSS: return "addss";
                case MInstOpcode::X86_ADDSD: return "addsd";
                default: return "";
            }
        }

        void emitLirMov(std::vector<MachineInstr>& lir, MachineOperand dst, MachineOperand src) {
            if (dst.isMem() && src.isMem()) {
                auto r10 = MachineOperand::createReg(REG_R10, dst.size);
                lir.push_back({ MInstOpcode::X86_MOVrm, { r10, src } });
                lir.push_back({ MInstOpcode::X86_MOVmr, { dst, r10 } });
            }
            else if (dst.isReg() && src.isMem()) {
                if (src.size < 4 && dst.size == src.size) {
                    MachineOperand extDst = MachineOperand::createReg(dst.reg, 4);
                    lir.push_back({ MInstOpcode::X86_MOVZX, { extDst, src } });
                }
                else {
                    lir.push_back({ MInstOpcode::X86_MOVrm, { dst, src } });
                }
            }
            else if (dst.isMem() && src.isReg()) {
                lir.push_back({ MInstOpcode::X86_MOVmr, { dst, src } });
            }
            else if (dst.isReg() && src.isImm()) {
                selectOptimalLoadImm(lir, dst, src.imm);
            }
            else if (dst.isMem() && src.isImm()) {
                lir.push_back({ MInstOpcode::X86_MOVmi, { dst, src } });
            }
            else if (dst.isReg() && src.isReg()) {
                if (src.size < 4 && dst.size > src.size) {
                    MachineOperand extDst = MachineOperand::createReg(dst.reg, 4);
                    lir.push_back({ MInstOpcode::X86_MOVZX, { extDst, src } });
                }
                else if (dst.reg != src.reg) {
                    lir.push_back({ MInstOpcode::X86_MOVrr, { dst, src } });
                }
            }
        }

        void emitLirBinary(std::vector<MachineInstr>& lir, MInstOpcode opReg, MInstOpcode opImm, MInstOpcode opMem, MachineOperand dst, MachineOperand src) {
            emitLirMov(lir, dst, dst); // Sanity check

            if (dst.isMem() && src.isMem()) {
                auto r11 = MachineOperand::createReg(REG_R11, dst.size);
                lir.push_back({ MInstOpcode::X86_MOVrm, { r11, src } });
                lir.push_back({ opMem, { dst, r11 } });
            }
            else if (src.isImm()) {
                lir.push_back({ opImm, { dst, src } });
            }
            else if (src.isMem()) {
                lir.push_back({ opMem, { dst, src } });
            }
            else {
                lir.push_back({ opReg, { dst, src } });
            }
        }

        void optimizePeephole(MIRFunction& mirFn) {
            bool changed = true;
            while (changed) {
                changed = false;
                for (auto& block : mirFn.blocks) {
                    std::vector<MachineInstr> newInsts;
                    for (size_t i = 0; i < block.insts.size(); ++i) {
                        auto& inst = block.insts[i];

                        if (inst.opcode == MInstOpcode::X86_MOVrr && inst.operands[0].reg == inst.operands[1].reg) {
                            changed = true;
                            continue;
                        }

                        if (inst.opcode == MInstOpcode::X86_LEAr && i + 1 < block.insts.size()) {
                            auto& next = block.insts[i + 1];
                            if (next.opcode == MInstOpcode::X86_MOVrr &&
                                inst.operands[1].isMem() &&
                                next.operands[0].reg == inst.operands[1].mem.baseReg &&
                                next.operands[1].reg == inst.operands[0].reg) {

                                MachineInstr addInst; 
                                addInst.opcode = MInstOpcode::X86_ADDri;
                                addInst.operands.push_back(next.operands[0]);
                                addInst.operands.push_back(MachineOperand::createImm(inst.operands[1].mem.offset, next.operands[0].size));

                                newInsts.push_back(addInst);
                                i++; changed = true; continue;
                            }
                        }

                        if (inst.opcode == MInstOpcode::X86_MOVrr && i + 2 < block.insts.size()) {
                            auto& math = block.insts[i + 1];
                            auto& movBack = block.insts[i + 2];

                            bool isMath = (math.opcode == MInstOpcode::X86_ADDri || math.opcode == MInstOpcode::X86_SUBri);
                            if (isMath && movBack.opcode == MInstOpcode::X86_MOVrr) {
                                if (math.operands[0].reg == inst.operands[0].reg &&
                                    movBack.operands[1].reg == inst.operands[0].reg &&
                                    movBack.operands[0].reg == inst.operands[1].reg) {

                                    MachineInstr directMath;
                                    directMath.opcode = math.opcode;
                                    directMath.operands.push_back(movBack.operands[0]);
                                    directMath.operands.push_back(math.operands[1]);

                                    newInsts.push_back(directMath);
                                    i += 2; changed = true; continue;
                                }
                            }
                        }

                        if (i + 1 < block.insts.size()) {
                            auto& next = block.insts[i + 1];

                            if (inst.opcode == MInstOpcode::X86_MOVrr && next.opcode == MInstOpcode::X86_MOVrr) {
                                if (inst.operands[0].reg == next.operands[1].reg && inst.operands[1].reg == next.operands[0].reg) {
                                    if (inst.operands[0].size == next.operands[0].size) {
                                        newInsts.push_back(inst);
                                        i++; changed = true; continue;
                                    }
                                }
                            }

                            if (inst.opcode == MInstOpcode::X86_MOVrr && next.opcode == MInstOpcode::X86_MOVrr) {
                                if (inst.operands[0].reg == next.operands[1].reg) {
                                    if (inst.operands[0].reg == REG_R10 || inst.operands[0].reg == REG_R11 || inst.operands[0].reg == REG_RAX) {
                                        newInsts.push_back({ MInstOpcode::X86_MOVrr, { next.operands[0], inst.operands[1] } });
                                        i++; changed = true; continue;
                                    }
                                }
                            }

                            if (inst.opcode == MInstOpcode::X86_MOVZX && next.opcode == MInstOpcode::X86_MOVZX) {
                                if (inst.operands[0].reg == next.operands[1].reg) {
                                    newInsts.push_back({ MInstOpcode::X86_MOVZX, { next.operands[0], inst.operands[1] } });
                                    i++; changed = true; continue;
                                }
                            }

                            if (inst.opcode == MInstOpcode::X86_MOVrm && next.opcode == MInstOpcode::X86_MOVrr) {
                                if (inst.operands[0].reg == REG_R10 && inst.operands[0].reg == next.operands[1].reg) {
                                    MachineOperand memSrc = inst.operands[1];
                                    MachineOperand finalDst = next.operands[0];

                                    newInsts.push_back({ MInstOpcode::X86_MOVrm, { finalDst, memSrc } });
                                    i++; changed = true; continue;
                                }
                            }

                            if (inst.opcode == MInstOpcode::X86_MOVZX && next.opcode == MInstOpcode::X86_MOVrr) {
                                if (inst.operands[0].reg == next.operands[1].reg) {
                                    MachineOperand newDst = next.operands[0];
                                    newInsts.push_back({ MInstOpcode::X86_MOVZX, { newDst, inst.operands[1] } });
                                    i++; changed = true; continue;
                                }
                            }

                            if (inst.opcode == MInstOpcode::X86_MOVri &&
                                (next.opcode == MInstOpcode::X86_MOVZX || next.opcode == MInstOpcode::X86_MOVrr)) {
                                if (inst.operands[0].reg == next.operands[1].reg) {
                                    if (inst.operands[0].reg == REG_R10 || inst.operands[0].reg == REG_R11 || inst.operands[0].reg == REG_RAX) {
                                        MachineOperand newDst = next.operands[0];
                                        MachineOperand newImm = MachineOperand::createImm(inst.operands[1].imm, newDst.size);

                                        newInsts.push_back({ MInstOpcode::X86_MOVri, { newDst, newImm } });
                                        i++;
                                        changed = true;
                                        continue;
                                    }
                                }
                            }
                        }
                        newInsts.push_back(inst);
                    }
                    block.insts = std::move(newInsts);
                }
            }
        }

        void genFunction(IRFunction& fn, Emitter& emitter) {
            if (fn.blocks.empty()) {
                return;
            }

            TargetRegisterInfo tri;
            tri.argRegs = argRegs;
            tri.returnReg = REG_RAX;
            tri.callerSaved = { REG_RAX, REG_RCX, REG_RDX, REG_R8, REG_R9 };
            tri.calleeSaved = { REG_RBX, REG_RSI, REG_RDI, REG_R12, REG_R13, REG_R14, REG_R15, REG_RBP };

            RegAlloc allocator;
            AllocResult alloc = allocator.allocate(fn, tri);

            MIRFunction mirFn;
            mirFn.name = fn.name;

            bool hasCall = false;
            bool hasAlloc = false;
            bool hasStackArgs = false;
            int localSlotsUsed = 0;
            int maxCallArgs = 0;

            std::map<int, uint64_t> constVals;
            std::set<int> usedAsReg;
            std::map<int, int> defCounts;
            std::map<int, int> useCounts;

            for (const auto& block : fn.blocks) {
                for (const auto& inst : block->instructions) {
                    if (inst.dest != -1) defCounts[inst.dest]++;
                    if (inst.src1 != -1) useCounts[inst.src1]++;
                    if (inst.src2 != -1) useCounts[inst.src2]++;
                    for (int arg : inst.args) if (arg != -1) useCounts[arg]++;
                }
            }

            for (const auto& block : fn.blocks) {
                for (const auto& inst : block->instructions) {
                    if (inst.op == OpCode::CONST && inst.dest != -1 && defCounts[inst.dest] == 1) {
                        constVals[inst.dest] = inst.imm;
                    }

                    if (inst.op == OpCode::CALL) {
                        hasCall = true;
                        if ((int)inst.args.size() > maxCallArgs) maxCallArgs = inst.args.size();
                        if (inst.label.empty() && inst.src1 != -1) usedAsReg.insert(inst.src1);
                        for (int arg : inst.args) if (arg != -1) usedAsReg.insert(arg);
                    }
                    else if (inst.op == OpCode::ALLOC) {
                        hasAlloc = true;
                    }
                    else if (inst.op == OpCode::LOAD_LOCAL || inst.op == OpCode::STORE_LOCAL || inst.op == OpCode::LEA_LOCAL) {
                        if ((int)inst.imm >= localSlotsUsed) localSlotsUsed = inst.imm + 1;
                        if (inst.src1 != -1) usedAsReg.insert(inst.src1);
                        if (inst.src2 != -1) usedAsReg.insert(inst.src2);
                    }
                    else if (inst.op == OpCode::GET_PARAM) {
                        if (inst.imm >= 4) hasStackArgs = true;
                    }
                    else {
                        if (inst.src1 != -1) usedAsReg.insert(inst.src1);
                        if (inst.src2 != -1) usedAsReg.insert(inst.src2);
                    }
                }
            }

            for (int reg : tri.calleeSaved) {
                for (auto const& [v, p] : alloc.registers) {
                    if (p == reg) {
                        if (constVals.count(v)) continue;
                        if (useCounts[v] == 0 && defCounts[v] == 0) continue;
                        mirFn.usedCalleeSaved.push_back(reg);
                        break;
                    }
                }
            }

            std::map<int, const Instruction*> defs;
            for (const auto& block : fn.blocks) {
                for (const auto& inst : block->instructions) {
                    if (inst.dest != -1) defs[inst.dest] = &inst;
                }
            }

            std::map<int, std::pair<int, int>> foldOffsets;
            for (const auto& block : fn.blocks) {
                for (const auto& inst : block->instructions) {
                    if ((inst.op == OpCode::LOAD || inst.op == OpCode::STORE) && !inst.isVolatile) {
                        int baseReg = inst.src1;
                        if (defs.count(baseReg) && useCounts[baseReg] == 1) {
                            const Instruction* defInst = defs[baseReg];
                            if (defInst->op == OpCode::ADD && defInst->src2 == -1) {
                                foldOffsets[baseReg] = { defInst->src1, (int)defInst->imm };
                            }
                        }
                    }
                }
            }

            std::map<int, const Instruction*> foldLoads;
            for (const auto& block : fn.blocks) {
                for (const auto& inst : block->instructions) {
                    if (inst.op == OpCode::TRUNC || inst.op == OpCode::CAST || inst.op == OpCode::ZEXT) {
                        int srcReg = inst.src1;
                        if (defs.count(srcReg) && useCounts[srcReg] == 1) {
                            const Instruction* defInst = defs[srcReg];
                            if (defInst->op == OpCode::LOAD && !defInst->isVolatile) {
                                foldLoads[srcReg] = defInst;
                            }
                        }
                    }
                }
            }

            bool omitFramePointer = true;
            bool needsRBP = !omitFramePointer;

            int calleeSavedSpace = mirFn.usedCalleeSaved.size() * 8;
            int maxLocals = std::max(localSlotsUsed, fn.localCount);
            int totalLocalsAndSpills = (maxLocals * 8) + alloc.spillSize;

            std::map<int, int> allocOffsets;
            int staticAllocSpace = 0;
            int currentAllocOffset = calleeSavedSpace + totalLocalsAndSpills;

            for (const auto& block : fn.blocks) {
                for (const auto& inst : block->instructions) {
                    if (inst.op == OpCode::ALLOC) {
                        uint64_t size = inst.imm;
                        uint64_t alignedSize = (size + 7) & ~7ULL;
                        currentAllocOffset += alignedSize;
                        allocOffsets[inst.dest] = currentAllocOffset;
                        staticAllocSpace += alignedSize;
                    }
                }
            }

            int shadowSpace = 0;
            if (hasCall) {
                shadowSpace = 32;
                if (maxCallArgs > 4) shadowSpace += (maxCallArgs - 4) * 8;
            }

            int finalStack = shadowSpace + totalLocalsAndSpills + staticAllocSpace;
            int numPushed = mirFn.usedCalleeSaved.size() + (needsRBP ? 1 : 0);
            int totalPushedSize = (numPushed * 8) + 8;

            int padding = 0;
            if (hasCall || finalStack > 0) {
                padding = (16 - ((totalPushedSize + finalStack) % 16)) % 16;
                finalStack += padding;
            }
            mirFn.frameSize = finalStack;
            int rbpToRspOffset = calleeSavedSpace + finalStack;
            auto createFrameMem = [&](int offset, int size) -> MachineOperand {
                if (omitFramePointer) {
                    return MachineOperand::createMem(REG_RSP, offset + rbpToRspOffset, size);
                }
                else {
                    return MachineOperand::createMem(REG_RBP, offset, size);
                }
            };

            auto resolveOp = [&](int vOp, int size) -> MachineOperand {
                if (vOp == -1) return MachineOperand::createImm(0, size);

                if (constVals.count(vOp)) {
                    return MachineOperand::createImm(constVals[vOp], size);
                }

                if (alloc.registers.count(vOp)) return MachineOperand::createReg(alloc.registers[vOp], size);
                if (alloc.spills.count(vOp)) {
                    int offset = -(calleeSavedSpace + (maxLocals * 8) + alloc.spills[vOp]);
                    return createFrameMem(offset, size);
                }
                return MachineOperand::createImm(0, size);
            };

            auto resolveSrc = [&](int vOp, int size) -> MachineOperand {
                if (vOp == -1) return MachineOperand::createImm(0, size);
                return resolveOp(vOp, size);
            };

            auto getLocalOffset = [&](int slot) -> int {
                return -(calleeSavedSpace + (slot * 8) + 8);
            };

            int globalIdx = 0;
            for (const auto& irBlock : fn.blocks) {
                MIRBasicBlock mb;
                mb.name = irBlock->name;
                const Instruction* prevInst = nullptr;

                for (const auto& inst : irBlock->instructions) {
                    switch (inst.op) {
                    case OpCode::GET_PARAM: {
                        int argIdx = inst.imm;
                        MachineOperand dst = resolveOp(inst.dest, inst.bytes);
                        if (argIdx < 4) {
                            auto argReg = MachineOperand::createReg(argRegs[argIdx], inst.bytes);
                            if (inst.bytes < 4) {
                                MachineOperand extDst = MachineOperand::createReg(dst.reg, 4);
                                mb.insts.push_back({ MInstOpcode::X86_MOVZX, { extDst, argReg } });
                            }
                            else {
                                emitLirMov(mb.insts, dst, argReg);
                            }
                        }
                        else {
                            int stackOffset = 16 + (argIdx * 8);
                            MachineOperand mem = createFrameMem(stackOffset, inst.bytes);
                            emitLirMov(mb.insts, dst, mem);
                        }
                        break;
                    }
                    case OpCode::INLINE_ASM: {
                        mb.insts.push_back({ MInstOpcode::X86_INLINE_ASM, { MachineOperand::createLabel(inst.label) } });
                        break;
                    }
                    case OpCode::MOV: {
                        MachineOperand src = resolveSrc(inst.src1, inst.bytes);
                        emitLirMov(mb.insts, resolveOp(inst.dest, inst.bytes), src);
                        break;
                    }
                    case OpCode::CONST: {
                        MachineOperand src = MachineOperand::createImm(inst.imm, inst.bytes);
                        emitLirMov(mb.insts, resolveOp(inst.dest, inst.bytes), src);
                        break;
                    }
                    case OpCode::LOAD_LOCAL: {
                        MachineOperand mem = createFrameMem(getLocalOffset(inst.imm), inst.bytes);
                        emitLirMov(mb.insts, resolveOp(inst.dest, inst.bytes), mem);
                        break;
                    }
                    case OpCode::STORE_LOCAL: {
                        MachineOperand mem = createFrameMem(getLocalOffset(inst.imm), inst.bytes);
                        emitLirMov(mb.insts, mem, resolveOp(inst.src1, inst.bytes));
                        break;
                    }
                    case OpCode::LEA_LOCAL: {
                        MachineOperand mem = createFrameMem(getLocalOffset(inst.imm), 8);
                        MachineOperand dst = resolveOp(inst.dest, 8);
                        if (dst.isMem()) {
                            auto r11 = MachineOperand::createReg(REG_R11, 8);
                            mb.insts.push_back({ MInstOpcode::X86_LEAr, { r11, mem } });
                            mb.insts.push_back({ MInstOpcode::X86_MOVmr, { dst, r11 } });
                        }
                        else {
                            mb.insts.push_back({ MInstOpcode::X86_LEAr, { dst, mem } });
                        }
                        break;
                    }
                    case OpCode::LOAD: {
                        if (foldLoads.count(inst.dest)) break;
                        int baseReg = inst.src1;
                        int offset = 0;
                        if (foldOffsets.count(baseReg)) {
                            offset = foldOffsets[baseReg].second;
                            baseReg = foldOffsets[baseReg].first;
                        }

                        MachineOperand srcMem = resolveOp(baseReg, 8);
                        if (srcMem.isMem()) {
                            auto r11 = MachineOperand::createReg(REG_R11, 8);
                            mb.insts.push_back({ MInstOpcode::X86_MOVrm, { r11, srcMem } });
                            srcMem = MachineOperand::createMem(REG_R11, offset, inst.bytes);
                        }
                        else if (srcMem.isImm()) {
                            auto r11 = MachineOperand::createReg(REG_R11, 8);
                            mb.insts.push_back({ MInstOpcode::X86_MOVri, { r11, srcMem } });
                            srcMem = MachineOperand::createMem(REG_R11, offset, inst.bytes);
                        }
                        else {
                            srcMem = MachineOperand::createMem(srcMem.reg, offset, inst.bytes);
                        }
                        emitLirMov(mb.insts, resolveOp(inst.dest, inst.bytes), srcMem);
                        break;
                    }
                    case OpCode::STORE: {
                        int baseReg = inst.src1;
                        int offset = 0;
                        if (foldOffsets.count(baseReg)) {
                            offset = foldOffsets[baseReg].second;
                            baseReg = foldOffsets[baseReg].first;
                        }

                        MachineOperand dstMem = resolveOp(baseReg, 8);
                        if (dstMem.isMem()) {
                            auto r11 = MachineOperand::createReg(REG_R11, 8);
                            mb.insts.push_back({ MInstOpcode::X86_MOVrm, { r11, dstMem } });
                            dstMem = MachineOperand::createMem(REG_R11, offset, inst.bytes);
                        }
                        else if (dstMem.isImm()) {
                            auto r11 = MachineOperand::createReg(REG_R11, 8);
                            mb.insts.push_back({ MInstOpcode::X86_MOVri, { r11, dstMem } });
                            dstMem = MachineOperand::createMem(REG_R11, offset, inst.bytes);
                        }
                        else {
                            dstMem = MachineOperand::createMem(dstMem.reg, offset, inst.bytes);
                        }

                        MachineOperand srcVal = inst.src2 == -1 ? MachineOperand::createImm(inst.imm, inst.bytes) : resolveOp(inst.src2, inst.bytes);
                        emitLirMov(mb.insts, dstMem, srcVal);
                        break;
                    }
                    case OpCode::LOAD_STR: {
                        MachineOperand dst = resolveOp(inst.dest, 8);
                        if (dst.isMem()) {
                            auto r11 = MachineOperand::createReg(REG_R11, 8);
                            mb.insts.push_back({ MInstOpcode::X86_LEAr, { r11, MachineOperand::createLabel(inst.label) } });
                            mb.insts.push_back({ MInstOpcode::X86_MOVmr, { dst, r11 } });
                        }
                        else {
                            mb.insts.push_back({ MInstOpcode::X86_LEAr, { dst, MachineOperand::createLabel(inst.label) } });
                        }
                        break;
                    }
                    case OpCode::CAST:
                    case OpCode::ZEXT:
                    case OpCode::TRUNC: {
                        int srcSize = (inst.imm > 0 && inst.imm <= 8) ? inst.imm : 8;
                        MachineOperand dst = resolveOp(inst.dest, inst.bytes);
                        MachineOperand src;

                        if (foldLoads.count(inst.src1)) {
                            const Instruction* loadInst = foldLoads[inst.src1];
                            int baseReg = loadInst->src1;
                            int offset = 0;

                            if (foldOffsets.count(baseReg)) {
                                offset = foldOffsets[baseReg].second;
                                baseReg = foldOffsets[baseReg].first;
                            }

                            MachineOperand baseOp = resolveOp(baseReg, 8);
                            int loadSize = loadInst->bytes;

                            if (inst.op == OpCode::TRUNC || (inst.op == OpCode::CAST && dst.size < loadSize)) {
                                loadSize = dst.size;
                            }

                            if (baseOp.isMem()) {
                                auto r11 = MachineOperand::createReg(REG_R11, 8);
                                mb.insts.push_back({ MInstOpcode::X86_MOVrm, { r11, baseOp } });
                                src = MachineOperand::createMem(REG_R11, offset, loadSize);
                            }
                            else if (baseOp.isImm()) {
                                auto r11 = MachineOperand::createReg(REG_R11, 8);
                                mb.insts.push_back({ MInstOpcode::X86_MOVri, { r11, baseOp } });
                                src = MachineOperand::createMem(REG_R11, offset, loadSize);
                            }
                            else {
                                src = MachineOperand::createMem(baseOp.reg, offset, loadSize);
                            }
                        }
                        else {
                            src = resolveSrc(inst.src1, srcSize);
                            if (inst.op == OpCode::TRUNC || (inst.op == OpCode::CAST && dst.size < src.size)) {
                                src.size = dst.size;
                            }
                        }

                        if (inst.op == OpCode::ZEXT || (inst.op == OpCode::CAST && dst.size > src.size)) {
                            if (src.isImm()) {
                                src.size = dst.size;
                                emitLirMov(mb.insts, dst, src);
                            }
                            else if (src.size == 4) {
                                emitLirMov(mb.insts, MachineOperand::createReg(dst.reg, 4), src);
                            }
                            else if (src.size < 4) {
                                MachineOperand extDst = MachineOperand::createReg(dst.reg, std::max(4, dst.size));
                                mb.insts.push_back({ MInstOpcode::X86_MOVZX, { extDst, src } });
                            }
                            else {
                                emitLirMov(mb.insts, dst, src);
                            }
                        }
                        else {
                            emitLirMov(mb.insts, dst, src);
                        }
                        break;
                    }
                    case OpCode::ALLOC: {
                        int offset = allocOffsets[inst.dest];
                        MachineOperand mem = createFrameMem(-offset, 8);
                        MachineOperand dst = resolveOp(inst.dest, 8);
                        if (dst.isMem()) {
                            auto r11 = MachineOperand::createReg(REG_R11, 8);
                            mb.insts.push_back({ MInstOpcode::X86_LEAr, { r11, mem } });
                            mb.insts.push_back({ MInstOpcode::X86_MOVmr, { dst, r11 } });
                        }
                        else {
                            mb.insts.push_back({ MInstOpcode::X86_LEAr, { dst, mem } });
                        }
                        break;
                    }
                    case OpCode::ADD:
                    case OpCode::SUB:
                    case OpCode::OR:
                    case OpCode::XOR: 
                    case OpCode::AND: {
                        if (inst.op == OpCode::ADD && foldOffsets.count(inst.dest)) break;

                        auto dst = resolveOp(inst.dest, inst.bytes);
                        auto src1 = resolveOp(inst.src1, inst.bytes);
                        auto src2 = inst.src2 == -1 ? MachineOperand::createImm(inst.imm, inst.bytes) : resolveOp(inst.src2, inst.bytes);

                        if (inst.op == OpCode::ADD && src2.isImm() && (int64_t)src2.imm >= -2147483648LL && (int64_t)src2.imm <= 2147483647LL) {
                            if (dst.isReg() && dst.size >= 4 && src1.isReg()) {
                                MachineOperand mem = MachineOperand::createMem(src1.reg, (int)src2.imm, dst.size);
                                mb.insts.push_back({ MInstOpcode::X86_LEAr, { dst, mem } });
                                break;
                            }
                        }

                        MachineOperand safe_src2 = src2;

                        if (src2.isReg() && dst.isReg() && src2.reg == dst.reg && (!src1.isReg() || src1.reg != dst.reg)) {
                            if (inst.op == OpCode::ADD || inst.op == OpCode::OR) {
                                safe_src2 = src1;
                                src1 = src2;
                            }
                            else {
                                auto r10 = MachineOperand::createReg(REG_R10, src2.size);
                                emitLirMov(mb.insts, r10, src2);
                                safe_src2 = r10;
                            }
                        }

                        emitLirMov(mb.insts, dst, src1);

                        if (dst.isMem() && safe_src2.isMem()) {
                            auto r10 = MachineOperand::createReg(REG_R10, safe_src2.size);
                            emitLirMov(mb.insts, r10, safe_src2);
                            safe_src2 = r10;
                        }

                        MInstOpcode opImm, opMem, opReg;
                        if (inst.op == OpCode::ADD) { opImm = MInstOpcode::X86_ADDri; opMem = MInstOpcode::X86_ADDmr; opReg = MInstOpcode::X86_ADDrr; }
                        else if (inst.op == OpCode::SUB) { opImm = MInstOpcode::X86_SUBri; opMem = MInstOpcode::X86_SUBmr; opReg = MInstOpcode::X86_SUBrr; }
                        else if (inst.op == OpCode::OR) { opImm = MInstOpcode::X86_ORri;  opMem = MInstOpcode::X86_ORmr;  opReg = MInstOpcode::X86_ORrr; }
                        else if (inst.op == OpCode::AND) { opImm = MInstOpcode::X86_ANDri; opMem = MInstOpcode::X86_ANDmr; opReg = MInstOpcode::X86_ANDrr; }
                        else { opImm = MInstOpcode::X86_XORri; opMem = MInstOpcode::X86_XORmr; opReg = MInstOpcode::X86_XORrr; }

                        if (safe_src2.isImm()) mb.insts.push_back({ opImm, { dst, safe_src2 } });
                        else if (safe_src2.isMem()) mb.insts.push_back({ opMem, { dst, safe_src2 } });
                        else mb.insts.push_back({ opReg, { dst, safe_src2 } });
                        break;
                    }
                    case OpCode::MUL: {
                        auto dst = resolveOp(inst.dest, inst.bytes);
                        auto src1 = resolveOp(inst.src1, inst.bytes);
                        auto src2 = inst.src2 == -1 ? MachineOperand::createImm(inst.imm, inst.bytes) : resolveOp(inst.src2, inst.bytes);

                        if (dst.isMem()) {
                            auto r10 = MachineOperand::createReg(REG_R10, inst.bytes);
                            emitLirMov(mb.insts, r10, src1);
                            if (src2.isImm()) mb.insts.push_back({ MInstOpcode::X86_IMULrri, { r10, r10, src2 } });
                            else mb.insts.push_back({ MInstOpcode::X86_IMULrr, { r10, src2 } });
                            emitLirMov(mb.insts, dst, r10);
                        }
                        else {
                            emitLirMov(mb.insts, dst, src1);
                            if (src2.isImm()) mb.insts.push_back({ MInstOpcode::X86_IMULrri, { dst, dst, src2 } });
                            else mb.insts.push_back({ MInstOpcode::X86_IMULrr, { dst, src2 } });
                        }
                        break;
                    }
                    case OpCode::DIV: {
                        auto src1 = resolveOp(inst.src1, inst.bytes);
                        auto src2 = resolveOp(inst.src2, inst.bytes);
                        auto rax = MachineOperand::createReg(REG_RAX, inst.bytes);

                        emitLirMov(mb.insts, rax, src1);
                        mb.insts.push_back({ MInstOpcode::X86_CQO, {} });

                        if (src2.isImm()) {
                            auto r10 = MachineOperand::createReg(REG_R10, inst.bytes);
                            emitLirMov(mb.insts, r10, src2);
                            mb.insts.push_back({ MInstOpcode::X86_IDIVr, { r10 } });
                        }
                        else {
                            mb.insts.push_back({ MInstOpcode::X86_IDIVr, { src2 } });
                        }
                        emitLirMov(mb.insts, resolveOp(inst.dest, inst.bytes), rax);
                        break;
                    }
                    case OpCode::SHL:
                    case OpCode::SHR: {
                        auto dst = resolveOp(inst.dest, inst.bytes);
                        auto src1 = resolveOp(inst.src1, inst.bytes);
                        auto src2 = inst.src2 == -1 ? MachineOperand::createImm(inst.imm, 1) : resolveOp(inst.src2, 1);

                        emitLirMov(mb.insts, dst, src1);
                        MInstOpcode opReg = (inst.op == OpCode::SHL) ? MInstOpcode::X86_SHLr : MInstOpcode::X86_SHRr;
                        MInstOpcode opCl = (inst.op == OpCode::SHL) ? MInstOpcode::X86_SHLcl : MInstOpcode::X86_SHRcl;

                        if (src2.isImm()) {
                            mb.insts.push_back({ opReg, { dst, src2 } });
                        }
                        else {
                            emitLirMov(mb.insts, MachineOperand::createReg(REG_RCX, 1), src2);
                            mb.insts.push_back({ opCl, { dst, MachineOperand::createReg(REG_RCX, 1) } });
                        }
                        break;
                    }
                    case OpCode::CMP_EQ:
                    case OpCode::CMP_NE:
                    case OpCode::CMP_LT:
                    case OpCode::CMP_GT:
                    case OpCode::CMP_LE:
                    case OpCode::CMP_GE: {
                        int opSize = 8;
                        if (defs.count(inst.src1)) {
                            opSize = defs[inst.src1]->bytes;
                        }

                        auto left = resolveOp(inst.src1, opSize);
                        auto right = inst.src2 == -1 ? MachineOperand::createImm(inst.imm, opSize) : resolveOp(inst.src2, opSize);

                        if (left.isMem() || left.isImm()) {
                            auto r10 = MachineOperand::createReg(REG_R10, opSize);
                            if (left.isImm()) {
                                mb.insts.push_back({ MInstOpcode::X86_MOVri, { r10, left } });
                            }
                            else {
                                mb.insts.push_back({ MInstOpcode::X86_MOVrm, { r10, left } });
                            }
                            left = r10;
                        }

                        if (right.isImm()) mb.insts.push_back({ MInstOpcode::X86_CMPri, { left, right } });
                        else if (right.isMem()) mb.insts.push_back({ MInstOpcode::X86_CMPrm, { left, right } });
                        else mb.insts.push_back({ MInstOpcode::X86_CMPrr, { left, right } });

                        if (inst.dest != -1) {
                            MInstOpcode setOp;
                            if (inst.op == OpCode::CMP_LT) setOp = MInstOpcode::X86_SETL;
                            else if (inst.op == OpCode::CMP_GT) setOp = MInstOpcode::X86_SETG;
                            else if (inst.op == OpCode::CMP_EQ) setOp = MInstOpcode::X86_SETE;
                            else if (inst.op == OpCode::CMP_NE) setOp = MInstOpcode::X86_SETNE;
                            else if (inst.op == OpCode::CMP_GE) setOp = MInstOpcode::X86_SETGE;
                            else if (inst.op == OpCode::CMP_LE) setOp = MInstOpcode::X86_SETLE;

                            auto dest8 = resolveOp(inst.dest, 1);
                            if (dest8.isMem()) {
                                auto r11b = MachineOperand::createReg(REG_R11, 1);
                                mb.insts.push_back({ setOp, { r11b } });
                                mb.insts.push_back({ MInstOpcode::X86_MOVmr, { dest8, r11b } });
                            }
                            else {
                                mb.insts.push_back({ setOp, { dest8 } });
                            }

                            if (inst.bytes > 1) {
                                auto destFull = resolveOp(inst.dest, inst.bytes);
                                MachineOperand extDst = MachineOperand::createReg(destFull.reg, std::max(4, destFull.size));
                                auto dest8_reg = dest8.isMem() ? MachineOperand::createReg(REG_R11, 1) : dest8;
                                mb.insts.push_back({ MInstOpcode::X86_MOVZX, { extDst, dest8_reg } });
                            }
                        }
                        break;
                    }
                    case OpCode::JMP: {
                        std::string targetLabel = ".L" + std::to_string(inst.imm);
                        for (const auto& b : fn.blocks) {
                            if (b->id == static_cast<int>(inst.imm)) { targetLabel = b->name; break; }
                        }
                        mb.insts.push_back({ MInstOpcode::X86_JMP, { MachineOperand::createLabel(targetLabel) } });
                        break;
                    }
                    case OpCode::JMP_FALSE: {
                        std::string targetLabel = ".L" + std::to_string(inst.imm);
                        for (const auto& b : fn.blocks) {
                            if (b->id == static_cast<int>(inst.imm)) { targetLabel = b->name; break; }
                        }

                        if (prevInst &&
                            (prevInst->op == OpCode::CMP_EQ || prevInst->op == OpCode::CMP_NE ||
                                prevInst->op == OpCode::CMP_LT || prevInst->op == OpCode::CMP_GT ||
                                prevInst->op == OpCode::CMP_LE || prevInst->op == OpCode::CMP_GE) &&
                            prevInst->dest == inst.src1) {

                            if (!mb.insts.empty() && mb.insts.back().opcode == MInstOpcode::X86_MOVZX) mb.insts.pop_back();
                            if (!mb.insts.empty() && mb.insts.back().opcode >= MInstOpcode::X86_SETL && mb.insts.back().opcode <= MInstOpcode::X86_SETLE) mb.insts.pop_back();

                            MInstOpcode jmpOp;
                            if (prevInst->op == OpCode::CMP_EQ) jmpOp = MInstOpcode::X86_JNE;
                            else if (prevInst->op == OpCode::CMP_NE) jmpOp = MInstOpcode::X86_JE;
                            else if (prevInst->op == OpCode::CMP_LT) jmpOp = MInstOpcode::X86_JGE;
                            else if (prevInst->op == OpCode::CMP_GT) jmpOp = MInstOpcode::X86_JLE;
                            else if (prevInst->op == OpCode::CMP_LE) jmpOp = MInstOpcode::X86_JG;
                            else jmpOp = MInstOpcode::X86_JL;

                            mb.insts.push_back({ jmpOp, { MachineOperand::createLabel(targetLabel) } });
                        }
                        else {
                            int opSize = inst.bytes;
                            if (defs.count(inst.src1)) opSize = defs[inst.src1]->bytes;

                            auto cond = resolveOp(inst.src1, opSize);
                            if (cond.isMem() || cond.isImm()) {
                                auto r10 = MachineOperand::createReg(REG_R10, std::max(opSize, 4));
                                if (cond.isImm()) mb.insts.push_back({ MInstOpcode::X86_MOVri, { r10, cond } });
                                else mb.insts.push_back({ MInstOpcode::X86_MOVrm, { r10, cond } });
                                mb.insts.push_back({ MInstOpcode::X86_TESTrr, { r10, r10 } });
                            }
                            else {
                                mb.insts.push_back({ MInstOpcode::X86_TESTrr, { cond, cond } });
                            }
                            mb.insts.push_back({ MInstOpcode::X86_JE, { MachineOperand::createLabel(targetLabel) } });
                        }
                        break;
                    }
                    case OpCode::CALL: {
                        if (alloc.callSpills.count(globalIdx)) {
                            for (int vReg : alloc.callSpills[globalIdx]) {
                                MachineOperand reg = resolveOp(vReg, 8);
                                MachineOperand mem = createFrameMem(-(calleeSavedSpace + (maxLocals * 8) + alloc.spills[vReg]), 8);
                                emitLirMov(mb.insts, reg, mem);
                            }
                        }

                        std::vector<MachineOperand> argSrcs;
                        for (size_t i = 0; i < inst.args.size(); ++i) {
                            argSrcs.push_back(resolveSrc(inst.args[i], inst.argBytes[i]));
                        }

                        MachineOperand callTarget;
                        if (inst.label.empty()) {
                            callTarget = resolveSrc(inst.src1, 8);
                        }

                        for (size_t i = 4; i < inst.args.size(); ++i) {
                            MachineOperand src = argSrcs[i];
                            int stackOffset = (i * 8);
                            MachineOperand mem = MachineOperand::createMem(REG_RSP, stackOffset, inst.argBytes[i]);
                            if (src.isImm()) mb.insts.push_back({ MInstOpcode::X86_MOVmi, { mem, src } });
                            else emitLirMov(mb.insts, mem, src);
                        }

                        int scratchRegs[] = { REG_R10, REG_R11, REG_RAX };
                        bool scratchUsed[3] = { false, false, false };

                        for (size_t i = 0; i < std::min((size_t)4, inst.args.size()); ++i) {
                            int targetReg = argRegs[i];
                            bool conflict = false;

                            for (size_t j = i + 1; j < std::min((size_t)4, inst.args.size()); ++j) {
                                if ((argSrcs[j].isReg() && argSrcs[j].reg == targetReg) ||
                                    (argSrcs[j].isMem() && argSrcs[j].mem.baseReg == targetReg)) {
                                    conflict = true; break;
                                }
                            }

                            if (inst.label.empty()) {
                                if ((callTarget.isReg() && callTarget.reg == targetReg) ||
                                    (callTarget.isMem() && callTarget.mem.baseReg == targetReg)) {
                                    conflict = true;
                                }
                            }

                            if (conflict) {
                                int chosenScratch = REG_RAX;
                                for (int sIdx = 0; sIdx < 3; ++sIdx) {
                                    int sr = scratchRegs[sIdx];
                                    if (scratchUsed[sIdx]) continue;

                                    bool used = false;
                                    for (size_t j = i + 1; j < std::min((size_t)4, inst.args.size()); ++j) {
                                        if (argSrcs[j].isReg() && argSrcs[j].reg == sr) used = true;
                                        if (argSrcs[j].isMem() && argSrcs[j].mem.baseReg == sr) used = true;
                                    }
                                    if (inst.label.empty()) {
                                        if (callTarget.isReg() && callTarget.reg == sr) used = true;
                                        if (callTarget.isMem() && callTarget.mem.baseReg == sr) used = true;
                                    }

                                    if (!used) {
                                        chosenScratch = sr;
                                        scratchUsed[sIdx] = true;
                                        break;
                                    }
                                }

                                MachineOperand scratchOp = MachineOperand::createReg(chosenScratch, 8);
                                mb.insts.push_back({ MInstOpcode::X86_MOVrr, { scratchOp, MachineOperand::createReg(targetReg, 8) } });

                                for (size_t j = i + 1; j < std::min((size_t)4, inst.args.size()); ++j) {
                                    if (argSrcs[j].isReg() && argSrcs[j].reg == targetReg) argSrcs[j].reg = chosenScratch;
                                    if (argSrcs[j].isMem() && argSrcs[j].mem.baseReg == targetReg) argSrcs[j].mem.baseReg = chosenScratch;
                                }
                                if (inst.label.empty()) {
                                    if (callTarget.isReg() && callTarget.reg == targetReg) callTarget.reg = chosenScratch;
                                    if (callTarget.isMem() && callTarget.mem.baseReg == targetReg) callTarget.mem.baseReg = chosenScratch;
                                }
                            }
                        }

                        for (size_t i = 0; i < std::min((size_t)4, inst.args.size()); ++i) {
                            MachineOperand src = argSrcs[i];
                            MachineOperand dstReg = MachineOperand::createReg(argRegs[i], std::max(4, inst.argBytes[i]));
                            if (src.isImm()) { mb.insts.push_back({ MInstOpcode::X86_MOVri, { dstReg, src } }); }
                            else if (inst.argBytes[i] < 4) { mb.insts.push_back({ MInstOpcode::X86_MOVZX, { dstReg, src } }); }
                            else { emitLirMov(mb.insts, dstReg, src); }
                        }

                        if (!inst.label.empty()) mb.insts.push_back({ MInstOpcode::X86_CALLpcrel, { MachineOperand::createLabel(inst.label) } });
                        else mb.insts.push_back({ MInstOpcode::X86_CALLr, { callTarget } });

                        if (inst.dest != -1) emitLirMov(mb.insts, resolveOp(inst.dest, 8), MachineOperand::createReg(REG_RAX, 8));

                        if (alloc.callSpills.count(globalIdx)) {
                            for (int vReg : alloc.callSpills[globalIdx]) {
                                MachineOperand reg = resolveOp(vReg, 8);
                                MachineOperand mem = MachineOperand::createMem(REG_RBP, -(calleeSavedSpace + (maxLocals * 8) + alloc.spills[vReg]), 8);
                                emitLirMov(mb.insts, reg, mem);
                            }
                        }
                        break;
                    }
                    case OpCode::RET: {
                        if (inst.src1 != -1) {
                            emitLirMov(mb.insts, MachineOperand::createReg(REG_RAX, 8), resolveSrc(inst.src1, 8));
                        }

                        if (needsRBP) {
                            if (calleeSavedSpace > 0) {
                                MachineOperand mem = MachineOperand::createMem(REG_RBP, -calleeSavedSpace, 8);
                                mb.insts.push_back({ MInstOpcode::X86_LEAr, { MachineOperand::createReg(REG_RSP, 8), mem } });
                            }
                            else {
                                mb.insts.push_back({ MInstOpcode::X86_MOVrr, { MachineOperand::createReg(REG_RSP, 8), MachineOperand::createReg(REG_RBP, 8) } });
                            }
                        }
                        else {
                            if (finalStack > 0) {
                                mb.insts.push_back({ MInstOpcode::X86_ADDri, { MachineOperand::createReg(REG_RSP, 8), MachineOperand::createImm(finalStack, 8) } });
                            }
                        }

                        for (auto it = mirFn.usedCalleeSaved.rbegin(); it != mirFn.usedCalleeSaved.rend(); ++it) {
                            mb.insts.push_back({ MInstOpcode::X86_POPr, { MachineOperand::createReg(*it, 8) } });
                        }

                        if (needsRBP) {
                            mb.insts.push_back({ MInstOpcode::X86_POPr, { MachineOperand::createReg(REG_RBP, 8) } });
                        }

                        mb.insts.push_back({ MInstOpcode::X86_RET, {} });
                        break;
                    }
                    default: break;
                    }
                    prevInst = &inst;
                    globalIdx++;
                }
                mirFn.blocks.push_back(std::move(mb));
            }

            removeUnreachableBlocks(mirFn);
            removeEmptyBlocks(mirFn);
            optimizePeephole(mirFn);

            emitter.emitLabel(sanitizeLabel(fn.name));

            if (needsRBP) {
                emitter.emitInstruction({ MInstOpcode::X86_PUSHr, { MachineOperand::createReg(REG_RBP, 8) } });
                emitter.emitInstruction({ MInstOpcode::X86_MOVrr, { MachineOperand::createReg(REG_RBP, 8), MachineOperand::createReg(REG_RSP, 8) } });
            }
            for (int reg : mirFn.usedCalleeSaved) {
                emitter.emitInstruction({ MInstOpcode::X86_PUSHr, { MachineOperand::createReg(reg, 8) } });
            }
            if (finalStack > 0) {
                emitter.emitInstruction({ MInstOpcode::X86_SUBri, { MachineOperand::createReg(REG_RSP, 8), MachineOperand::createImm(finalStack, 8) } });
            }

            for (size_t b = 0; b < mirFn.blocks.size(); ++b) {
                const auto& mb = mirFn.blocks[b];
                emitter.emitLabel(sanitizeLabel(mb.name));

                for (const auto& inst : mb.insts) {
                    if (inst.opcode == MInstOpcode::X86_JMP) {
                        if (b + 1 < mirFn.blocks.size() && inst.operands[0].label == mirFn.blocks[b + 1].name) {
                            continue;
                        }
                    }
                    emitter.emitInstruction(inst);
                }
            }
        }
    };

    std::unique_ptr<CodeGen> createWin64() { return std::make_unique<CodeGenWin64>(); }
}