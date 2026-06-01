#include "../include/irgen.hpp"
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <string>

namespace gbpp {

    int IRGenerator::getOffset(Type* type, const std::string& field) {
        std::string structName = type->name;
        if (type->isPointer()) structName = type->base->name;

        if (m_structMap.count(structName)) {
            for (auto& f : m_structMap[structName]->fields) {
                if (f.name == field) return f.offset;
            }
        }
        return 0;
    }

    IRModule IRGenerator::generate(const Program& prog) {
        m_module = IRModule();
        m_structMap.clear();
        m_loopExits.clear();
        m_globals.clear();

        for (const auto& v : prog.globalVars) {
            m_globals[v->name] = v.get();
        }

        for (const auto& st : prog.structs) {
            if (st->genericParams.empty()) {
                m_structMap[st->name] = st.get();
            }
        }

        for (const auto& fn : prog.functions) {
            if (fn->genericParams.empty()) {
                genFunction(*fn);
            }
        }

        return std::move(m_module);
    }

    void IRGenerator::emit(Instruction inst) {
        if (m_currentBlock) m_currentBlock->instructions.push_back(inst);
    }

    void IRGenerator::genFunction(const FunctionDecl& fn) {
        if (!fn.body) return;
        IRFunction irFn;
        irFn.name = fn.name;
        irFn.argCount = (int)fn.params.size();

        for (const auto& attr : fn.attributes) {
            if (attr == "inline") irFn.isInline = true;
            if (attr == "export") irFn.isExported = true;
        }

        m_module.functions.push_back(std::move(irFn));
        m_currentFunc = &m_module.functions.back();

        m_locals.clear();
        m_stackSlots.clear();
        m_stackPrimitives.clear();

        m_currentBlock = m_currentFunc->createBlock(".L_entry_" + fn.name);
        m_exitBlock = m_currentFunc->createBlock(".L_exit_" + fn.name);
        m_retReg = (fn.returnTypeResolved && fn.returnTypeResolved != &TypeVoid) ? m_currentFunc->allocVReg() : -1;

        for (size_t i = 0; i < fn.params.size(); ++i) {
            int vReg = m_currentFunc->allocVReg();
            m_locals[fn.params[i].name] = vReg;

            if (!fn.params[i].resolvedType) {
                std::cerr << "[IRGen Fatal Error] Unresolved type for parameter '" << fn.params[i].name << "' in function '" << fn.name << "'.\n";
                exit(1);
            }

            int size = fn.params[i].resolvedType->sizeBytes;
            emit({ OpCode::GET_PARAM, vReg, -1, -1, (uint64_t)i, size });
        }

        genBlock(*fn.body);

        if (m_currentBlock && (m_currentBlock->instructions.empty() ||
            (m_currentBlock->instructions.back().op != OpCode::RET &&
                m_currentBlock->instructions.back().op != OpCode::JMP))) {
            emit({ OpCode::JMP, -1, -1, -1, (uint64_t)m_exitBlock->id });
        }

        m_currentBlock = m_exitBlock;
        if (m_retReg != -1) {
            emit({ OpCode::RET, -1, m_retReg });
        }
        else {
            emit({ OpCode::RET, -1, -1 });
        }

        if (m_exitBlock->instructions.empty()) {
            // can be cleaned up in optimization phase
        }

        for (size_t i = 0; i < m_currentFunc->blocks.size(); ++i) {
            if (m_currentFunc->blocks[i].get() == m_exitBlock) {
                auto ptr = std::move(m_currentFunc->blocks[i]);
                m_currentFunc->blocks.erase(m_currentFunc->blocks.begin() + i);
                m_currentFunc->blocks.push_back(std::move(ptr));
                break;
            }
        }
    }

    void IRGenerator::genBlock(const BlockStmt& block) {
        for (const auto& stmt : block.statements) {
            if (!m_currentBlock) break;
            genStmt(*stmt);
        }
    }

    void IRGenerator::genStmt(const Stmt& stmt) {
        if (auto block = dynamic_cast<const BlockStmt*>(&stmt)) {
            genBlock(*block);
        }
        else if (auto asmStmt = dynamic_cast<const AsmStmt*>(&stmt)) {
            Instruction inst;
            inst.op = OpCode::INLINE_ASM;
            inst.label = asmStmt->assembly;
            emit(inst);
        }
        else if (auto decl = dynamic_cast<const VarDecl*>(&stmt)) {
            if (!decl->resolvedType) {
                std::cerr << "[IRGen Fatal Error] Unresolved type for variable '" << decl->name << "'.\n";
                exit(1);
            }
            int size = decl->resolvedType ? decl->resolvedType->sizeBytes : 8;
            bool isStruct = decl->resolvedType && decl->resolvedType->scalar == ScalarType::Struct;
            bool isArray = decl->resolvedType && decl->resolvedType->isArray;
            bool isStackPrimitive = decl->attributes.count("stack") > 0 && !isStruct && !isArray;

            if (isStruct && m_structMap.count(decl->resolvedType->name)) {
                int maxOff = 0;
                for (const auto& field : m_structMap[decl->resolvedType->name]->fields) {
                    maxOff = std::max(maxOff, field.offset + 8);
                }
                size = maxOff;
            }

            if (isStackPrimitive) {
                m_stackPrimitives.insert(decl->name);
            }

            if (isStruct || isArray || isStackPrimitive) {
                int varReg = m_currentFunc->allocVReg();
                m_locals[decl->name] = varReg;

                if (isStackPrimitive) {
                    emit({ OpCode::ALLOC, varReg, -1, -1, (uint64_t)size });
                    if (decl->initializer) {
                        int val = genExpr(*decl->initializer);
                        emit({ OpCode::STORE, -1, varReg, val, 0, size });
                    }
                }
                else if (decl->initializer) {
                    int val = genExpr(*decl->initializer);
                    emit({ OpCode::MOV, varReg, val, -1, 0, 8 });
                }
                else if (isArray && size == 0) {
                    std::string tName = decl->resolvedType->name;
                    size_t bracketPos = tName.find('[');
                    std::string sizeStr = tName.substr(bracketPos + 1, tName.size() - bracketPos - 2);

                    int sizeVarReg;
                    if (m_locals.count(sizeStr)) sizeVarReg = m_locals[sizeStr];
                    else if (m_stackSlots.count(sizeStr)) {
                        sizeVarReg = m_currentFunc->allocVReg();
                        emit({ OpCode::LOAD_LOCAL, sizeVarReg, -1, -1, (uint64_t)m_stackSlots[sizeStr], 8 });
                    }
                    else { std::cerr << "Dynamic array error\n"; exit(1); }

                    int elemSize = decl->resolvedType->base ? decl->resolvedType->base->sizeBytes : 1;
                    if (elemSize > 1) {
                        int totalSizeReg = m_currentFunc->allocVReg();
                        emit({ OpCode::MUL, totalSizeReg, sizeVarReg, -1, (uint64_t)elemSize, 8 });
                        sizeVarReg = totalSizeReg;
                    }

                    emit({ OpCode::ALLOC, varReg, sizeVarReg, -1, 0 });
                }
                else if (size > 0) {
                    emit({ OpCode::ALLOC, varReg, -1, -1, (uint64_t)size });
                }
            }
            else {
                int varReg = m_currentFunc->allocVReg();
                m_locals[decl->name] = varReg;

                if (decl->initializer) {
                    int val = genExpr(*decl->initializer);
                    emit({ OpCode::MOV, varReg, val, -1, 0, size });
                }
                else {
                    emit({ OpCode::CONST, varReg, -1, -1, 0, size });
                }
            }
        }
        else if (auto ret = dynamic_cast<const ReturnStmt*>(&stmt)) {
            if (ret->value && m_retReg != -1) {
                int val = genExpr(*ret->value);
                int size = ret->value->type ? ret->value->type->sizeBytes : 8;
                emit({ OpCode::MOV, m_retReg, val, -1, 0, size });
            }
            emit({ OpCode::JMP, -1, -1, -1, (uint64_t)m_exitBlock->id });
            m_currentBlock = nullptr;
        }
        else if (auto expr = dynamic_cast<const ExprStmt*>(&stmt)) {
            genExpr(*expr->expr);
        }
        else if (auto ifStmt = dynamic_cast<const IfStmt*>(&stmt)) {
            int cond = genExpr(*ifStmt->condition);

            BasicBlock* thenBlock = m_currentFunc->createBlock();
            BasicBlock* elseBlock = ifStmt->elseBranch ? m_currentFunc->createBlock() : nullptr;
            BasicBlock* endBlock = m_currentFunc->createBlock();

            emit({ OpCode::JMP_FALSE, -1, cond, -1, (uint64_t)(elseBlock ? elseBlock->id : endBlock->id) });
            emit({ OpCode::JMP, -1, -1, -1, (uint64_t)thenBlock->id });

            m_currentBlock = thenBlock;
            genStmt(*ifStmt->thenBranch);
            if (m_currentBlock) emit({ OpCode::JMP, -1, -1, -1, (uint64_t)endBlock->id });

            if (elseBlock) {
                m_currentBlock = elseBlock;
                genStmt(*ifStmt->elseBranch);
                if (m_currentBlock) emit({ OpCode::JMP, -1, -1, -1, (uint64_t)endBlock->id });
            }

            m_currentBlock = endBlock;
        }
        else if (auto whileStmt = dynamic_cast<const WhileStmt*>(&stmt)) {
            BasicBlock* condBlock = m_currentFunc->createBlock();
            BasicBlock* bodyBlock = m_currentFunc->createBlock();
            BasicBlock* endBlock = m_currentFunc->createBlock();

            emit({ OpCode::JMP, -1, -1, -1, (uint64_t)condBlock->id });

            m_currentBlock = condBlock;
            int cond = genExpr(*whileStmt->condition);
            emit({ OpCode::JMP_FALSE, -1, cond, -1, (uint64_t)endBlock->id });
            emit({ OpCode::JMP, -1, -1, -1, (uint64_t)bodyBlock->id });

            m_loopExits.push_back(endBlock);

            m_currentBlock = bodyBlock;
            genStmt(*whileStmt->body);

            m_loopExits.pop_back();

            if (m_currentBlock) emit({ OpCode::JMP, -1, -1, -1, (uint64_t)condBlock->id });

            m_currentBlock = endBlock;
        }
        else if (auto forStmt = dynamic_cast<const ForStmt*>(&stmt)) {
            if (forStmt->init) {
                genStmt(*forStmt->init);
            }

            BasicBlock* condBlock = m_currentFunc->createBlock();
            BasicBlock* bodyBlock = m_currentFunc->createBlock();
            BasicBlock* updateBlock = m_currentFunc->createBlock();
            BasicBlock* endBlock = m_currentFunc->createBlock();

            emit({ OpCode::JMP, -1, -1, -1, (uint64_t)condBlock->id });

            m_currentBlock = condBlock;
            if (forStmt->condition) {
                int cond = genExpr(*forStmt->condition);
                emit({ OpCode::JMP_FALSE, -1, cond, -1, (uint64_t)endBlock->id });
            }
            emit({ OpCode::JMP, -1, -1, -1, (uint64_t)bodyBlock->id });

            m_loopExits.push_back(endBlock);

            m_currentBlock = bodyBlock;
            if (forStmt->body) {
                genStmt(*forStmt->body);
            }

            m_loopExits.pop_back();

            if (m_currentBlock) {
                emit({ OpCode::JMP, -1, -1, -1, (uint64_t)updateBlock->id });
            }

            m_currentBlock = updateBlock;
            if (forStmt->update) {
                genExpr(*forStmt->update);
            }
            emit({ OpCode::JMP, -1, -1, -1, (uint64_t)condBlock->id });

            m_currentBlock = endBlock;
        }
        else if (auto breakStmt = dynamic_cast<const BreakStmt*>(&stmt)) {
            if (!m_loopExits.empty()) {
                emit({ OpCode::JMP, -1, -1, -1, (uint64_t)m_loopExits.back()->id });
                m_currentBlock = nullptr;
            }
        }
    }

    int IRGenerator::genExpr(const Expr& expr) {
        if (auto lit = dynamic_cast<const IntLiteral*>(&expr)) {
            int d = m_currentFunc->allocVReg();
            int size = lit->type ? lit->type->sizeBytes : 8;
            uint64_t val = 0;
            try {
                std::string txt = lit->value;
                int base = (txt.starts_with("0x") || txt.starts_with("0X")) ? 16 : 10;
                val = std::stoull(txt, nullptr, base);
            }
            catch (...) { val = 0; }
            emit({ OpCode::CONST, d, -1, -1, val, size });
            return d;
        }
        else if (auto fLit = dynamic_cast<const FloatLiteral*>(&expr)) {
            int d = m_currentFunc->allocVReg();
            int size = fLit->type ? fLit->type->sizeBytes : 8;
            uint64_t val = 0;
            try {
                if (size == 4) {
                    float f = std::stof(fLit->value);
                    std::memcpy(&val, &f, sizeof(float));
                }
                else {
                    double dbl = std::stod(fLit->value);
                    std::memcpy(&val, &dbl, sizeof(double));
                }
            }
            catch (...) {}
            emit({ OpCode::CONST, d, -1, -1, val, size });
            return d;
        }
        else if (auto nullLit = dynamic_cast<const NullLiteral*>(&expr)) {
            int d = m_currentFunc->allocVReg();
            emit({ OpCode::CONST, d, -1, -1, 0, 8 });
            return d;
        }
        else if (auto sInit = dynamic_cast<const StructInitExpr*>(&expr)) {
            int dest = m_currentFunc->allocVReg();
            int size = 8;
            if (m_structMap.count(sInit->structName)) {
                int maxOff = 0;
                for (const auto& field : m_structMap[sInit->structName]->fields) {
                    maxOff = std::max(maxOff, field.offset + 8);
                }
                size = maxOff;
            }
            emit({ OpCode::ALLOC, dest, -1, -1, (uint64_t)size });

            for (const auto& fInit : sInit->fields) {
                int valReg = genExpr(*fInit.value);
                int offset = getOffset(sInit->type, fInit.name);

                int addrReg = m_currentFunc->allocVReg();
                emit({ OpCode::ADD, addrReg, dest, -1, (uint64_t)offset, 8 });

                int fSize = fInit.value->type ? fInit.value->type->sizeBytes : 8;
                emit({ OpCode::STORE, -1, addrReg, valReg, 0, fSize });
            }
            return dest;
        }
        else if (auto str = dynamic_cast<const StringLiteral*>(&expr)) {
            int vReg = m_currentFunc->allocVReg();
            int strIdx = (int)m_module.readOnlyStrings.size();
            m_module.readOnlyStrings.push_back(str->value);
            Instruction inst;
            inst.op = OpCode::LOAD_STR;
            inst.dest = vReg;
            inst.label = "str_" + std::to_string(strIdx);
            inst.bytes = 8;
            emit(inst);
            return vReg;
        }
        else if (auto var = dynamic_cast<const VarExpr*>(&expr)) {
            if (m_globals.count(var->name)) {
                const VarDecl* gDecl = m_globals[var->name];
                if (gDecl->resolvedType && gDecl->resolvedType->isConst && gDecl->initializer) {
                    if (!dynamic_cast<const StructInitExpr*>(gDecl->initializer.get())) {
                        return genExpr(*gDecl->initializer);
                    }
                }
            }

            int res = m_currentFunc->allocVReg();
            int size = 8;
            if (var->type && !var->type->isArray && var->type->scalar != ScalarType::Struct) {
                size = var->type->sizeBytes;
            }

            if (m_locals.count(var->name)) {
                if (m_stackPrimitives.count(var->name)) {
                    Instruction inst = { OpCode::LOAD, res, m_locals[var->name], -1, 0, size };
                    if (var->type && var->type->isVolatile) inst.isVolatile = true;
                    emit(inst);
                }
                else {
                    emit({ OpCode::MOV, res, m_locals[var->name], -1, 0, size });
                }
            }
            else {
                Instruction inst;
                inst.op = OpCode::LOAD_STR;
                inst.dest = res;
                inst.label = var->name;
                inst.bytes = 8;
                emit(inst);
            }

            return res;
        }
        else if (auto enumAcc = dynamic_cast<const EnumAccessExpr*>(&expr)) {
            int d = m_currentFunc->allocVReg();
            int size = enumAcc->type ? enumAcc->type->sizeBytes : 8;
            emit({ OpCode::CONST, d, -1, -1, enumAcc->value, size });
            return d;
        }
        else if (auto un = dynamic_cast<const UnaryExpr*>(&expr)) {
            int d = m_currentFunc->allocVReg();
            int operand = genExpr(*un->operand);
            int size = un->type ? un->type->sizeBytes : 8;

            if (un->op == TokenType::Minus) {
                int zeroReg = m_currentFunc->allocVReg();
                emit({ OpCode::CONST, zeroReg, -1, -1, 0, size });
                emit({ OpCode::SUB, d, zeroReg, operand, 0, size });
            }
            else if (un->op == TokenType::Bang) {
                int zeroReg = m_currentFunc->allocVReg();
                emit({ OpCode::CONST, zeroReg, -1, -1, 0, size });
                emit({ OpCode::CMP_EQ, d, operand, zeroReg, 0, size });
            }
            else if (un->op == TokenType::Tilde) {
                int onesReg = m_currentFunc->allocVReg();
                uint64_t mask = (size == 8) ? (uint64_t)-1 : ((1ULL << (size * 8)) - 1);
                emit({ OpCode::CONST, onesReg, -1, -1, mask, size });
                emit({ OpCode::XOR, d, operand, onesReg, 0, size });
            }
            return d;
        }
        else if (auto arr = dynamic_cast<const ArrayAccessExpr*>(&expr)) {
            int base = genExpr(*arr->array);
            int index = genExpr(*arr->index);
            int index64 = m_currentFunc->allocVReg();
            emit({ OpCode::CAST, index64, index, -1, 8 });

            int elementSize = arr->type ? arr->type->sizeBytes : 1;
            int scaledIndex = index64;
            if (elementSize > 1) {
                scaledIndex = m_currentFunc->allocVReg();
                emit({ OpCode::MUL, scaledIndex, index64, -1, (uint64_t)elementSize, 8 });
            }

            int addr = m_currentFunc->allocVReg();
            emit({ OpCode::ADD, addr, base, scaledIndex });

            int res = m_currentFunc->allocVReg();

            Instruction inst = { OpCode::LOAD, res, addr, -1, 0, elementSize };
            if (arr->type && arr->type->isVolatile) inst.isVolatile = true;
            emit(inst);

            return res;
        }
        else if (auto alloc = dynamic_cast<const AllocExpr*>(&expr)) {
            int oneReg = m_currentFunc->allocVReg();
            emit({ OpCode::CONST, oneReg, -1, -1, 1, 8 });
            uint64_t structSize = alloc->resolvedTargetType->sizeBytes;

            int sizeReg = m_currentFunc->allocVReg();
            emit({ OpCode::CONST, sizeReg, -1, -1, structSize, 8 });

            int dest = m_currentFunc->allocVReg();

            Instruction inst;
            inst.op = OpCode::CALL;
            inst.dest = dest;
            inst.label = "calloc";
            inst.args = { oneReg, sizeReg };
            inst.argBytes = { 8, 8 };
            emit(inst);

            if (!alloc->initMethodName.empty()) {
                std::vector<int> argRegs;
                std::vector<int> argBytes;

                argRegs.push_back(dest);
                argBytes.push_back(8);

                for (auto& arg : alloc->args) {
                    argRegs.push_back(genExpr(*arg));
                    argBytes.push_back(arg->type ? arg->type->sizeBytes : 8);
                }

                Instruction initCall = { OpCode::CALL, -1, -1, -1, 0 };
                initCall.args = argRegs;
                initCall.argBytes = argBytes;
                initCall.label = alloc->initMethodName;
                emit(initCall);
            }

            return dest;
        }
        else if (auto sizeExpr = dynamic_cast<const SizeofExpr*>(&expr)) {
            int d = m_currentFunc->allocVReg();
            uint64_t structSize = sizeExpr->resolvedTargetType->sizeBytes;

            emit({ OpCode::CONST, d, -1, -1, structSize, 8 });
            return d;
        }
        else if (auto mem = dynamic_cast<const MemberExpr*>(&expr)) {
            int base = -1;

            if (auto varObj = dynamic_cast<const VarExpr*>(mem->object.get())) {
                if (m_globals.count(varObj->name)) {
                    const VarDecl* gDecl = m_globals[varObj->name];
                    if (gDecl->resolvedType && gDecl->resolvedType->isConst && gDecl->initializer) {
                        if (auto sInit = dynamic_cast<const StructInitExpr*>(gDecl->initializer.get())) {
                            for (const auto& fInit : sInit->fields) {
                                if (fInit.name == mem->memberName) {
                                    return genExpr(*fInit.value);
                                }
                            }
                        }
                    }
                }
            }

            if (auto deref = dynamic_cast<const DerefExpr*>(mem->object.get())) {
                base = genExpr(*deref->operand);
            }
            else {
                base = genExpr(*mem->object);
            }

            if (base == -1) {
                std::cerr << "[Internal Error] Failed to lower base for member access: " << mem->memberName << "\n";
                exit(1);
            }

            int offset = getOffset(mem->object->type, mem->memberName);
            int addr = m_currentFunc->allocVReg();

            if (offset > 0) {
                emit({ OpCode::ADD, addr, base, -1, (uint64_t)offset, 8 });
            }
            else {
                emit({ OpCode::MOV, addr, base, -1, 0, 8 });
            }

            if (mem->type && mem->type->isArray) return addr;

            int res = m_currentFunc->allocVReg();
            int size = (mem->type) ? mem->type->sizeBytes : 8;

            Instruction inst = { OpCode::LOAD, res, addr, -1, 0, size };
            if (mem->type && mem->type->isVolatile) inst.isVolatile = true;
            emit(inst);

            return res;
        }
        else if (auto deref = dynamic_cast<const DerefExpr*>(&expr)) {
            int addr = genExpr(*deref->operand);
            int res = m_currentFunc->allocVReg();
            int size = deref->type ? deref->type->sizeBytes : 8;

            Instruction inst = { OpCode::LOAD, res, addr, -1, 0, size };
            if (deref->type && deref->type->isVolatile) inst.isVolatile = true;
            emit(inst);

            return res;
        }
        else if (auto assign = dynamic_cast<const AssignmentExpr*>(&expr)) {
            int val = genExpr(*assign->value);
            int size = (assign->type) ? assign->type->sizeBytes : 8;

            auto getFinalVal = [&](int currReg) {
                if (assign->op == TokenType::Equal) return val;

                int res = m_currentFunc->allocVReg();
                bool isF = assign->type && assign->type->isFloatingPoint();
                OpCode op = OpCode::ADD;

                if (assign->op == TokenType::PlusEqual) op = isF ? OpCode::FADD : OpCode::ADD;
                else if (assign->op == TokenType::MinusEqual) op = isF ? OpCode::FSUB : OpCode::SUB;
                else if (assign->op == TokenType::StarEqual) op = isF ? OpCode::FMUL : OpCode::MUL;
                else if (assign->op == TokenType::SlashEqual) op = isF ? OpCode::FDIV : OpCode::DIV;

                emit({ op, res, currReg, val, 0, size });
                return res;
            };

            if (auto assignMem = dynamic_cast<const MemberExpr*>(assign->target.get())) {
                int base;
                if (auto deref = dynamic_cast<const DerefExpr*>(assignMem->object.get())) base = genExpr(*deref->operand);
                else base = genExpr(*assignMem->object);

                int offset = getOffset(assignMem->object->type, assignMem->memberName);
                int addr = m_currentFunc->allocVReg();

                if (offset > 0) emit({ OpCode::ADD, addr, base, -1, (uint64_t)offset, 8 });
                else emit({ OpCode::MOV, addr, base, -1, 0, 8 });

                int finalVal = val;
                if (assign->op != TokenType::Equal) {
                    int curr = m_currentFunc->allocVReg();
                    Instruction loadInst = { OpCode::LOAD, curr, addr, -1, 0, size };
                    if (assignMem->type && assignMem->type->isVolatile) loadInst.isVolatile = true;
                    emit(loadInst);
                    finalVal = getFinalVal(curr);
                }

                Instruction storeInst = { OpCode::STORE, -1, addr, finalVal, 0, size };
                if (assignMem->type && assignMem->type->isVolatile) storeInst.isVolatile = true;
                emit(storeInst);
                return finalVal;
            }
            if (auto assignArr = dynamic_cast<const ArrayAccessExpr*>(assign->target.get())) {
                int base = genExpr(*assignArr->array);
                int index = genExpr(*assignArr->index);

                int index64 = m_currentFunc->allocVReg();
                emit({ OpCode::CAST, index64, index, -1, 8 });

                int elementSize = assignArr->type ? assignArr->type->sizeBytes : 1;
                int scaledIndex = index64;
                if (elementSize > 1) {
                    scaledIndex = m_currentFunc->allocVReg();
                    emit({ OpCode::MUL, scaledIndex, index64, -1, (uint64_t)elementSize, 8 });
                }

                int addr = m_currentFunc->allocVReg();
                emit({ OpCode::ADD, addr, base, scaledIndex });

                int finalVal = val;
                if (assign->op != TokenType::Equal) {
                    int curr = m_currentFunc->allocVReg();
                    Instruction loadInst = { OpCode::LOAD, curr, addr, -1, 0, size };
                    if (assignArr->type && assignArr->type->isVolatile) loadInst.isVolatile = true;
                    emit(loadInst);
                    finalVal = getFinalVal(curr);
                }

                Instruction storeInst = { OpCode::STORE, -1, addr, finalVal, 0, size };
                if (assignArr->type && assignArr->type->isVolatile) storeInst.isVolatile = true;
                emit(storeInst);
                return finalVal;
            }
            if (auto assignVar = dynamic_cast<const VarExpr*>(assign->target.get())) {
                int storeSize = size;
                if (assign->target->type && (assign->target->type->isArray || assign->target->type->scalar == ScalarType::Struct)) {
                    storeSize = 8;
                }

                int finalVal = val;
                if (assign->op != TokenType::Equal) {
                    int curr = m_currentFunc->allocVReg();
                    if (m_stackPrimitives.count(assignVar->name)) {
                        Instruction loadInst = { OpCode::LOAD, curr, m_locals[assignVar->name], -1, 0, storeSize };
                        if (assignVar->type && assignVar->type->isVolatile) loadInst.isVolatile = true;
                        emit(loadInst);
                    }
                    else {
                        emit({ OpCode::MOV, curr, m_locals[assignVar->name], -1, 0, storeSize });
                    }
                    finalVal = getFinalVal(curr);
                }

                if (m_stackPrimitives.count(assignVar->name)) {
                    Instruction storeInst = { OpCode::STORE, -1, m_locals[assignVar->name], finalVal, 0, storeSize };
                    if (assignVar->type && assignVar->type->isVolatile) storeInst.isVolatile = true;
                    emit(storeInst);
                }
                else emit({ OpCode::MOV, m_locals[assignVar->name], finalVal, -1, 0, storeSize });
                return finalVal;
            }

            if (auto assignDeref = dynamic_cast<const DerefExpr*>(assign->target.get())) {
                int addr = genExpr(*assignDeref->operand);

                int finalVal = val;
                if (assign->op != TokenType::Equal) {
                    int curr = m_currentFunc->allocVReg();
                    Instruction loadInst = { OpCode::LOAD, curr, addr, -1, 0, size };
                    if (assignDeref->type && assignDeref->type->isVolatile) loadInst.isVolatile = true;
                    emit(loadInst);
                    finalVal = getFinalVal(curr);
                }

                Instruction storeInst = { OpCode::STORE, -1, addr, finalVal, 0, size };
                if (assignDeref->type && assignDeref->type->isVolatile) storeInst.isVolatile = true;
                emit(storeInst);
                return finalVal;
            }
        }
        else if (auto addrOf = dynamic_cast<const AddrOfExpr*>(&expr)) {
            const Expr* target = addrOf->operand.get();

            if (auto var = dynamic_cast<const VarExpr*>(target)) {
                int res = m_currentFunc->allocVReg();
                if (m_locals.count(var->name)) {
                    if (var->type && (var->type->isArray || var->type->scalar == ScalarType::Struct)) {
                        emit({ OpCode::MOV, res, m_locals[var->name], -1, 0, 8 });
                        return res;
                    }
                    if (m_stackPrimitives.count(var->name)) {
                        emit({ OpCode::MOV, res, m_locals[var->name], -1, 0, 8 });
                        return res;
                    }
                    std::cerr << "[IRGen Error] Cannot take address of primitive... Use [[@stack]] attribute.\n";
                    exit(1);
                }
            }
            else if (auto mem = dynamic_cast<const MemberExpr*>(target)) {
                int base;
                if (auto deref = dynamic_cast<const DerefExpr*>(mem->object.get())) {
                    base = genExpr(*deref->operand);
                }
                else {
                    base = genExpr(*mem->object);
                }

                int offset = getOffset(mem->object->type, mem->memberName);

                int addr = m_currentFunc->allocVReg();
                emit({ OpCode::ADD, addr, base, -1, (uint64_t)offset, 8 });

                return addr;
            }
            else if (auto arr = dynamic_cast<const ArrayAccessExpr*>(target)) {
                int base = genExpr(*arr->array);
                int index = genExpr(*arr->index);

                int index64 = m_currentFunc->allocVReg();
                emit({ OpCode::CAST, index64, index, -1, 8 });

                int elementSize = arr->type ? arr->type->sizeBytes : 1;
                int scaledIndex = index64;
                if (elementSize > 1) {
                    scaledIndex = m_currentFunc->allocVReg();
                    emit({ OpCode::MUL, scaledIndex, index64, -1, (uint64_t)elementSize, 8 });
                }

                int addr = m_currentFunc->allocVReg();
                emit({ OpCode::ADD, addr, base, scaledIndex });

                return addr;
            }
            else if (auto deref = dynamic_cast<const DerefExpr*>(target)) {
                return genExpr(*deref->operand);
            }

            std::cerr << "[IRGen Error] Invalid operand for address-of (&).\n";
            exit(1);
        }
        else if (auto bin = dynamic_cast<const BinaryExpr*>(&expr)) {
            int l = genExpr(*bin->left);
            int r = genExpr(*bin->right);
            int d = m_currentFunc->allocVReg();

            bool isFloat = bin->type && bin->type->isFloatingPoint();
            int size = bin->type ? bin->type->sizeBytes : 8;
            OpCode op = OpCode::ADD;

            switch (bin->op) {
                case TokenType::Plus:
                    op = isFloat ? OpCode::FADD : OpCode::ADD;
                    break;
                case TokenType::Minus:
                    op = isFloat ? OpCode::FSUB : OpCode::SUB;
                    break;
                case TokenType::Star:
                    op = isFloat ? OpCode::FMUL : OpCode::MUL;
                    break;
                case TokenType::Slash:
                    op = isFloat ? OpCode::FDIV : OpCode::DIV;
                    break;
                case TokenType::LT:
                    op = OpCode::CMP_LT;
                    break;
                case TokenType::GT:
                    op = OpCode::CMP_GT;
                    break;
                case TokenType::LE:
                    op = OpCode::CMP_LE;
                    break;
                case TokenType::GE:
                    op = OpCode::CMP_GE;
                    break;
                case TokenType::EqualEqual:
                    op = OpCode::CMP_EQ;
                    break;
                case TokenType::NotEqual:
                    op = OpCode::CMP_NE;
                    break;
                case TokenType::Pipe:
                    op = OpCode::OR;
                    break;
                case TokenType::Ampersand:
                    op = OpCode::AND;
                    break;
                case TokenType::ShiftLeft:
                    op = OpCode::SHL;
                    break;
                case TokenType::ShiftRight:
                    op = OpCode::SHR;
                    break;
                default: break;
            }

            emit({ op, d, l, r, 0, static_cast<int>(size) });
            return d;
        }

        else if (auto call = dynamic_cast<const CallExpr*>(&expr)) {
            std::vector<int> argRegs;
            std::vector<int> argBytes;

            for (auto& arg : call->args) {
                argRegs.push_back(genExpr(*arg));
                argBytes.push_back(arg->type ? arg->type->sizeBytes : 8);
            }

            int dest = -1;
            bool isVoid = (call->type && call->type->scalar == ScalarType::Void);
            if (!isVoid) dest = m_currentFunc->allocVReg();

            Instruction inst = { OpCode::CALL, dest, -1, -1, 0, (uint64_t)(call->type ? call->type->sizeBytes : 8) };
            inst.args = argRegs;
            inst.argBytes = argBytes;

            if (auto var = dynamic_cast<const VarExpr*>(call->callee.get())) {
                if (m_locals.find(var->name) == m_locals.end()) {
                    inst.label = var->name;
                }
                else {
                    inst.src1 = genExpr(*call->callee);
                }
            }
            else {
                inst.src1 = genExpr(*call->callee);
            }

            emit(inst);
            return dest != -1 ? dest : 0;
        }
        else if (auto cast = dynamic_cast<const CastExpr*>(&expr)) {
            if (auto fLit = dynamic_cast<const FloatLiteral*>(cast->operand.get())) {
                int d = m_currentFunc->allocVReg();
                int destSize = cast->targetType->sizeBytes;
                uint64_t val = 0;
                try {
                    if (destSize == 4) {
                        float f = std::stof(fLit->value);
                        std::memcpy(&val, &f, sizeof(float));
                    }
                    else {
                        double dbl = std::stod(fLit->value);
                        std::memcpy(&val, &dbl, sizeof(double));
                    }
                }
                catch (...) {}
                emit({ OpCode::CONST, d, -1, -1, val, destSize });
                return d;
            }

            int src = genExpr(*cast->operand);
            int dest = m_currentFunc->allocVReg();
            int srcSize = cast->operand->type ? cast->operand->type->sizeBytes : 8;
            int destSize = cast->targetType->sizeBytes;

            if (cast->castKind == CastKind::Bits || srcSize == destSize) {
                emit({ OpCode::MOV, dest, src, -1, 0, destSize });
            }
            else {
                Instruction inst;
                inst.op = (destSize > srcSize) ? OpCode::ZEXT : OpCode::TRUNC;
                inst.dest = dest;
                inst.src1 = src;
                inst.bytes = destSize;
                inst.imm = srcSize;
                emit(inst);
            }
            return dest;
        }
        std::cerr << "[IRGen Error] Unsupported expression type encountered!\n";
        exit(1);
    }

    void IRGenerator::optimize(IRModule& mod, bool enableOpt) {
        if (!enableOpt) return;

        bool globalPassChanged = true;
        while (globalPassChanged) {
            globalPassChanged = false;








            bool inlinedAnything = true;
            while (inlinedAnything) {
                inlinedAnything = false;
                for (auto& fn : mod.functions) {
                    for (auto& block : fn.blocks) {
                        std::vector<Instruction> newInsts;

                        for (auto& inst : block->instructions) {
                            if (inst.op == OpCode::CALL && !inst.label.empty()) {
                                IRFunction* target = nullptr;
                                for (auto& t : mod.functions) {
                                    if (t.name == inst.label) { target = &t; break; }
                                }

                                if (target && (target->isInline || target->blocks.size() <= 4)) {
                                    bool hasBranches = false;
                                    int instCount = 0;
                                    for (auto& tblock : target->blocks) {
                                        instCount += tblock->instructions.size();
                                        for (auto& tinst : tblock->instructions) {
                                            if (tinst.op == OpCode::JMP_FALSE) hasBranches = true;
                                        }
                                    }

                                    if (!hasBranches && instCount <= 30) {
                                        std::map<int, int> vregMap;
                                        for (auto& tblock : target->blocks) {
                                            for (auto& tinst : tblock->instructions) {
                                                if (tinst.op == OpCode::GET_PARAM) {
                                                    vregMap[tinst.dest] = inst.args[tinst.imm];
                                                }
                                                else if (tinst.op == OpCode::RET) {
                                                    if (inst.dest != -1 && tinst.src1 != -1) {
                                                        int retSrc = vregMap.count(tinst.src1) ? vregMap[tinst.src1] : tinst.src1;
                                                        newInsts.push_back({ OpCode::MOV, inst.dest, retSrc, -1, 0, inst.bytes });
                                                    }
                                                }
                                                else if (tinst.op != OpCode::JMP) {
                                                    Instruction cpy = tinst;
                                                    if (cpy.dest != -1) {
                                                        cpy.dest = fn.allocVReg();
                                                        vregMap[tinst.dest] = cpy.dest;
                                                    }
                                                    if (cpy.src1 != -1 && vregMap.count(cpy.src1)) cpy.src1 = vregMap[cpy.src1];
                                                    if (cpy.src2 != -1 && vregMap.count(cpy.src2)) cpy.src2 = vregMap[cpy.src2];

                                                    for (size_t i = 0; i < cpy.args.size(); ++i) {
                                                        if (vregMap.count(cpy.args[i])) cpy.args[i] = vregMap[cpy.args[i]];
                                                    }
                                                    newInsts.push_back(cpy);
                                                }
                                            }
                                        }
                                        inlinedAnything = true;
                                        continue;
                                    }
                                }
                            }
                            newInsts.push_back(inst);
                        }
                        block->instructions = std::move(newInsts);
                    }
                }
            }











            std::unordered_set<std::string> reachable;
            std::vector<std::string> worklist;

            reachable.insert("main");
            reachable.insert("_start");
            worklist.push_back("main");
            worklist.push_back("_start");

            // protect fn's with @export
            for (const auto& fn : mod.functions) {
                if (fn.isExported) {
                    if (reachable.insert(fn.name).second) {
                        worklist.push_back(fn.name);
                    }
                }
            }

            while (!worklist.empty()) {
                std::string current = worklist.back();
                worklist.pop_back();

                for (auto& fn : mod.functions) {
                    if (fn.name == current) {
                        for (auto& block : fn.blocks) {
                            for (auto& inst : block->instructions) {
                                if (inst.op == OpCode::CALL && !inst.label.empty()) {
                                    if (reachable.insert(inst.label).second) {
                                        worklist.push_back(inst.label);
                                    }
                                }
                                if (inst.op == OpCode::LOAD_STR && !inst.label.empty()) {
                                    if (reachable.insert(inst.label).second) {
                                        worklist.push_back(inst.label);
                                    }
                                }
                            }
                        }
                    }
                }
            }

            std::vector<IRFunction> activeFuncs;
            for (auto& fn : mod.functions) {
                if (reachable.count(fn.name)) {
                    activeFuncs.push_back(std::move(fn));
                }
            }
            mod.functions = std::move(activeFuncs);







            auto hasSideEffects = [](const Instruction& inst) {
                if (inst.isVolatile) return true;
                OpCode op = inst.op;
                return op == OpCode::STORE || op == OpCode::STORE_LOCAL ||
                    op == OpCode::CALL || op == OpCode::RET ||
                    op == OpCode::JMP || op == OpCode::JMP_FALSE ||
                    op == OpCode::INLINE_ASM || op == OpCode::ALLOC;
            };

            auto isCmp = [](OpCode op) {
                return op == OpCode::CMP_EQ || op == OpCode::CMP_NE ||
                    op == OpCode::CMP_LT || op == OpCode::CMP_GT ||
                    op == OpCode::CMP_LE || op == OpCode::CMP_GE;
            };

            for (auto& fn : mod.functions) {
                bool changed = true;
                bool fnPass2Changed = false;
                while (changed) { 
                    changed = false;

                    std::map<int, int> useCounts;
                    std::map<int, int> defCounts;
                    std::map<int, int> localUses;
                    std::map<int, int> regSizes;

                    for (auto& block : fn.blocks) {
                        for (auto& inst : block->instructions) {
                            if (inst.src1 != -1) useCounts[inst.src1]++;
                            if (inst.src2 != -1) useCounts[inst.src2]++;
                            for (int arg : inst.args) useCounts[arg]++;
                            if (inst.op == OpCode::LOAD_LOCAL || inst.op == OpCode::LEA_LOCAL) {
                                localUses[static_cast<int>(inst.imm)]++;
                            }

                            if (inst.dest != -1) {
                                defCounts[inst.dest]++;
                                if (inst.op == OpCode::CAST || inst.op == OpCode::LOAD || inst.op == OpCode::GET_PARAM) {
                                    regSizes[inst.dest] = inst.bytes;
                                }
                                else if (inst.bytes > 0) {
                                    regSizes[inst.dest] = inst.bytes;
                                }
                                else if (inst.src1 != -1 && regSizes.count(inst.src1)) {
                                    regSizes[inst.dest] = regSizes[inst.src1];
                                }
                                else {
                                    regSizes[inst.dest] = 8;
                                }
                            }
                        }
                    }

                    std::map<int, uint64_t> globalConsts;
                    std::map<int, int> globalAliases;
                    for (auto& block : fn.blocks) {
                        for (auto& inst : block->instructions) {
                            if (inst.dest != -1 && defCounts[inst.dest] == 1) {
                                if (inst.op == OpCode::CONST) {
                                    globalConsts[inst.dest] = inst.imm;
                                }
                                else if (inst.op == OpCode::MOV && inst.src1 != -1 && defCounts[inst.src1] == 1) {
                                    globalAliases[inst.dest] = inst.src1;
                                }
                            }
                        }
                    }

                    for (auto& block : fn.blocks) {
                        std::map<int, int> aliases;
                        std::map<int, uint64_t> constants;
                        std::map<ExprVal, int> availableExprs;

                        struct CmpInfo { OpCode op; int src1; int src2; uint64_t imm; };
                        std::map<int, CmpInfo> cmpSources;
                        std::map<int, int> localToReg;
                        std::map<int, int> ptrBase;
                        std::map<int, uint64_t> ptrOffset;

                        std::map<int, std::map<uint64_t, int>> memStateReg;
                        std::map<int, std::map<uint64_t, uint64_t>> memStateImm;
                        std::map<int, std::map<uint64_t, int>> memStateSize;
                        std::map<int, std::map<uint64_t, int>> lastStoreIdx;

                        std::map<int, int> lastRegDefIdx;
                        std::vector<Instruction> newInsts;
                        std::vector<bool> deadInsts;

                        for (auto& inst : block->instructions) {
                            if (inst.dest != -1) {
                                constants.erase(inst.dest);
                                aliases.erase(inst.dest);
                                ptrBase.erase(inst.dest);
                                ptrOffset.erase(inst.dest);

                                for (auto it = availableExprs.begin(); it != availableExprs.end(); ) {
                                    if (it->first.src1 == inst.dest || it->first.src2 == inst.dest || it->second == inst.dest) {
                                        it = availableExprs.erase(it);
                                    }
                                    else {
                                        ++it;
                                    }
                                }
                            }

                            if (isCmp(inst.op)) {
                                if (inst.dest != -1) cmpSources[inst.dest] = { inst.op, inst.src1, inst.src2, inst.imm };
                            }

                            int origSrc1 = inst.src1;
                            int origSrc2 = inst.src2;
                            std::vector<int> origArgs = inst.args;

                            auto resolve = [&](int reg) {
                                int curr = reg;
                                std::unordered_set<int> seen;
                                while (aliases.count(curr) || globalAliases.count(curr)) {
                                    if (!seen.insert(curr).second) break;
                                    curr = aliases.count(curr) ? aliases[curr] : globalAliases[curr];
                                }
                                return curr;
                            };

                            if (inst.src1 != -1) {
                                int newSrc = resolve(inst.src1);
                                if (newSrc != inst.src1) {
                                    useCounts[newSrc]++;
                                    inst.src1 = newSrc;
                                    changed = true;
                                }

                                if (constants.count(inst.src1) && inst.dest != -1) {
                                    constants[inst.dest] = constants[inst.src1];
                                }
                            }

                            if (inst.src2 != -1) {
                                int newSrc = resolve(inst.src2);
                                if (newSrc != inst.src2) {
                                    useCounts[newSrc]++;
                                    inst.src2 = newSrc;
                                    changed = true;
                                }
                            }

                            auto isConst = [&](int src, uint64_t imm) {
                                return (src == -1) || (constants.count(src) > 0) || (globalConsts.count(src) > 0);
                            };

                            auto getConst = [&](int src, uint64_t imm) -> uint64_t {
                                if (src == -1) return imm;
                                if (constants.count(src)) return constants[src];
                                return globalConsts[src];
                            };

                            if (inst.op == OpCode::CONST && inst.dest != -1) {
                                constants[inst.dest] = inst.imm;
                            }

                            if (inst.op == OpCode::STORE_LOCAL) {
                                if (inst.src1 != -1) localToReg[inst.imm] = inst.src1;
                            }
                            else if (inst.op == OpCode::LOAD_LOCAL) {
                                if (localToReg.count(inst.imm)) {
                                    inst.op = OpCode::MOV;
                                    inst.src1 = localToReg[inst.imm];
                                    inst.imm = 0;
                                    changed = true;
                                }
                                else {
                                    localToReg[inst.imm] = inst.dest;
                                }
                            }

                            if (inst.op == OpCode::MOV && inst.dest != -1 && inst.src1 != -1) {
                                aliases[inst.dest] = inst.src1;
                                if (constants.count(inst.src1)) {
                                    constants[inst.dest] = constants[inst.src1];
                                }
                            }

                            if (inst.op == OpCode::CAST) {
                                int srcSize = regSizes.count(inst.src1) ? regSizes[inst.src1] : 8;
                                if (srcSize == inst.bytes) {
                                    inst.op = OpCode::MOV;
                                    inst.imm = 0;
                                    changed = true;
                                }
                            }

                            if ((inst.op == OpCode::ADD || inst.op == OpCode::MUL || inst.op == OpCode::OR)
                                && isConst(inst.src1, 0) && !isConst(inst.src2, inst.imm)) {
                                std::swap(inst.src1, inst.src2);
                                changed = true;
                            }

                            if (inst.op == OpCode::ADD || inst.op == OpCode::SUB || inst.op == OpCode::OR) {
                                if (!isConst(inst.src1, 0) && isConst(inst.src2, inst.imm)) {
                                    if (getConst(inst.src2, inst.imm) == 0) {
                                        inst.op = OpCode::MOV;
                                        inst.src2 = -1;
                                        inst.imm = 0;
                                        changed = true;
                                    }
                                }
                            }
                            if (inst.op == OpCode::MUL) {
                                if (!isConst(inst.src1, 0) && isConst(inst.src2, inst.imm)) {
                                    uint64_t c = getConst(inst.src2, inst.imm);
                                    if (c == 1) {
                                        inst.op = OpCode::MOV;
                                        inst.src2 = -1;
                                        inst.imm = 0;
                                        changed = true;
                                    }
                                    else if (c == 0) {
                                        inst.op = OpCode::CONST;
                                        inst.src1 = -1;
                                        inst.src2 = -1;
                                        inst.imm = 0;
                                        changed = true;
                                    }
                                }
                            }

                            if (inst.op == OpCode::ADD || inst.op == OpCode::SUB ||
                                inst.op == OpCode::MUL || inst.op == OpCode::DIV ||
                                inst.op == OpCode::SHL || inst.op == OpCode::SHR ||
                                inst.op == OpCode::OR || inst.op == OpCode::XOR ||
                                inst.op == OpCode::AND) {

                                if (isConst(inst.src1, 0) && isConst(inst.src2, inst.imm)) {
                                    uint64_t v1 = getConst(inst.src1, 0);
                                    uint64_t v2 = getConst(inst.src2, inst.imm);
                                    uint64_t res = 0;
                                    bool canFold = true;

                                    switch (inst.op) {
                                    case OpCode::ADD: res = v1 + v2; break;
                                    case OpCode::SUB: res = v1 - v2; break;
                                    case OpCode::MUL: res = v1 * v2; break;
                                    case OpCode::DIV:
                                        if (v2 != 0) res = v1 / v2;
                                        else canFold = false;
                                        break;
                                    case OpCode::SHL: res = v1 << v2; break;
                                    case OpCode::SHR: res = v1 >> v2; break;
                                    case OpCode::OR:  res = v1 | v2; break;
                                    case OpCode::XOR: res = v1 ^ v2; break;
                                    case OpCode::AND: res = v1 & v2; break;
                                    default: canFold = false; break;
                                    }

                                    if (canFold) {
                                        inst.op = OpCode::CONST;
                                        inst.src1 = -1;
                                        inst.src2 = -1;
                                        inst.imm = res;
                                        changed = true;
                                        if (inst.dest != -1) constants[inst.dest] = res;
                                    }
                                }
                            }

                            if (inst.op == OpCode::CAST && isConst(inst.src1, 0)) {
                                inst.op = OpCode::CONST;
                                inst.imm = getConst(inst.src1, 0);
                                inst.src1 = -1;
                                changed = true;
                            }

                            if (inst.src2 != -1 && isConst(inst.src2, 0)) {
                                if (inst.op == OpCode::ADD || inst.op == OpCode::SUB ||
                                    inst.op == OpCode::MUL || inst.op == OpCode::OR ||
                                    inst.op == OpCode::XOR || inst.op == OpCode::AND ||
                                    inst.op == OpCode::SHL || inst.op == OpCode::SHR ||
                                    inst.op == OpCode::CMP_EQ || inst.op == OpCode::CMP_NE ||
                                    inst.op == OpCode::CMP_LT || inst.op == OpCode::CMP_GT ||
                                    inst.op == OpCode::CMP_LE || inst.op == OpCode::CMP_GE ||
                                    inst.op == OpCode::STORE) {

                                    inst.imm = getConst(inst.src2, 0);
                                    inst.src2 = -1;
                                    changed = true;
                                }
                            }

                            if (inst.op == OpCode::CMP_EQ || inst.op == OpCode::CMP_NE ||
                                inst.op == OpCode::CMP_LT || inst.op == OpCode::CMP_GT ||
                                inst.op == OpCode::CMP_LE || inst.op == OpCode::CMP_GE) {

                                if (isConst(inst.src1, 0) && isConst(inst.src2, inst.imm)) {
                                    int64_t v1 = getConst(inst.src1, 0);
                                    int64_t v2 = getConst(inst.src2, inst.imm);
                                    uint64_t res = 0;

                                    switch (inst.op) {
                                        case OpCode::CMP_EQ: res = (v1 == v2); break;
                                        case OpCode::CMP_NE: res = (v1 != v2); break;
                                        case OpCode::CMP_LT: res = (v1 < v2); break;
                                        case OpCode::CMP_GT: res = (v1 > v2); break;
                                        case OpCode::CMP_LE: res = (v1 <= v2); break;
                                        case OpCode::CMP_GE: res = (v1 >= v2); break;
                                        default: break;
                                    }
                                    inst.op = OpCode::CONST;
                                    inst.src1 = -1;
                                    inst.src2 = -1;
                                    inst.imm = res;
                                    inst.bytes = 1;
                                    changed = true;
                                    if (inst.dest != -1) constants[inst.dest] = res;
                                }
                            }

                            if (inst.op == OpCode::JMP_FALSE && isConst(inst.src1, 0)) {
                                uint64_t cond = getConst(inst.src1, 0);
                                if (cond == 0) {
                                    inst.op = OpCode::JMP;
                                    inst.src1 = -1;
                                    changed = true;
                                }
                                else {
                                    changed = true;
                                    continue;
                                }
                            }

                            if (inst.op == OpCode::CONST || inst.op == OpCode::ADD || inst.op == OpCode::SUB ||
                                inst.op == OpCode::MUL || inst.op == OpCode::DIV ||
                                inst.op == OpCode::CAST || inst.op == OpCode::LEA_LOCAL ||
                                inst.op == OpCode::CMP_EQ || inst.op == OpCode::CMP_NE ||
                                inst.op == OpCode::CMP_LT || inst.op == OpCode::CMP_GT ||
                                inst.op == OpCode::CMP_LE || inst.op == OpCode::CMP_GE ||
                                inst.op == OpCode::OR || inst.op == OpCode::XOR ||
                                inst.op == OpCode::SHL || inst.op == OpCode::SHR) {

                                ExprVal ev{ inst.op, inst.src1, inst.src2, inst.imm };
                                if (availableExprs.count(ev)) {
                                    inst.op = OpCode::MOV;
                                    useCounts[availableExprs[ev]]++;
                                    inst.src1 = availableExprs[ev];
                                    inst.src2 = -1;
                                    inst.imm = 0;
                                    changed = true;
                                }
                                else if (inst.dest != -1) {
                                    availableExprs[ev] = inst.dest;
                                }
                            }










                            if (inst.op == OpCode::ADD && inst.src2 == -1) {
                                int base = inst.src1;
                                uint64_t offset = inst.imm;
                                if (ptrBase.count(base)) {
                                    offset += ptrOffset[base];
                                    base = ptrBase[base];
                                }
                                ptrBase[inst.dest] = base;
                                ptrOffset[inst.dest] = offset;
                            }
                            else if (inst.op == OpCode::ALLOC || inst.op == OpCode::GET_PARAM) {
                                ptrBase[inst.dest] = inst.dest;
                                ptrOffset[inst.dest] = 0;
                            }
                            else if ((inst.op == OpCode::MOV || inst.op == OpCode::CAST) && inst.src1 != -1) {
                                if (ptrBase.count(inst.src1)) {
                                    ptrBase[inst.dest] = ptrBase[inst.src1];
                                    ptrOffset[inst.dest] = ptrOffset[inst.src1];
                                }
                            }
                            else if (inst.op == OpCode::CALL) {
                                if (inst.dest != -1) {
                                    ptrBase[inst.dest] = inst.dest;
                                    ptrOffset[inst.dest] = 0;
                                }
                                for (size_t i = 0; i < inst.args.size(); ++i) {
                                    int newArg = resolve(inst.args[i]);
                                    if (newArg != inst.args[i]) { inst.args[i] = newArg; changed = true; }
                                }
                                memStateReg.clear();
                                memStateImm.clear();
                                lastStoreIdx.clear();
                            }

                            if (inst.op == OpCode::STORE && inst.src1 != -1) {
                                int base = inst.src1;
                                uint64_t offset = 0;
                                bool knownAlias = false;

                                if (ptrBase.count(inst.src1)) {
                                    base = ptrBase[inst.src1];
                                    offset = ptrOffset[inst.src1];
                                    knownAlias = true;
                                }

                                if (knownAlias && !inst.isVolatile) {
                                    if (lastStoreIdx[base].count(offset)) {
                                        if (memStateSize[base].count(offset) && memStateSize[base][offset] == inst.bytes) {
                                            int deadIdx = lastStoreIdx[base][offset];
                                            deadInsts[deadIdx] = true;
                                            changed = true;
                                        }
                                    }

                                    auto& regMap = memStateReg[base];
                                    auto& immMap = memStateImm[base];
                                    auto& sizeMap = memStateSize[base];
                                    auto& storeMap = lastStoreIdx[base];

                                    for (auto it = sizeMap.begin(); it != sizeMap.end(); ) {
                                        uint64_t existingOffset = it->first;
                                        uint64_t existingSize = it->second;

                                        bool overlap = (offset < existingOffset + existingSize) && (offset + inst.bytes > existingOffset);

                                        if (overlap) {
                                            if (offset <= existingOffset && (offset + inst.bytes) >= (existingOffset + existingSize)) {
                                                if (storeMap.count(existingOffset)) {
                                                    int deadIdx = storeMap[existingOffset];
                                                    deadInsts[deadIdx] = true;
                                                    changed = true;
                                                }
                                            }

                                            regMap.erase(existingOffset);
                                            immMap.erase(existingOffset);
                                            storeMap.erase(existingOffset);
                                            it = sizeMap.erase(it);
                                        }
                                        else {
                                            ++it;
                                        }
                                    }

                                    if (inst.src2 != -1) {
                                        regMap[offset] = inst.src2;
                                    }
                                    else {
                                        immMap[offset] = inst.imm;
                                    }
                                    sizeMap[offset] = inst.bytes;
                                    storeMap[offset] = newInsts.size();
                                }
                                else {
                                    memStateReg.clear(); memStateImm.clear(); memStateSize.clear(); lastStoreIdx.clear();
                                }
                            }
                            else if (inst.op == OpCode::LOAD && inst.src1 != -1) {
                                int base = inst.src1;
                                uint64_t offset = 0;
                                bool knownAlias = false;

                                if (ptrBase.count(inst.src1)) {
                                    base = ptrBase[inst.src1];
                                    offset = ptrOffset[inst.src1];
                                    knownAlias = true;
                                }

                                bool canForward = knownAlias && !inst.isVolatile && memStateSize[base].count(offset) && memStateSize[base][offset] == inst.bytes;

                                if (canForward && memStateReg[base].count(offset)) {
                                    inst.op = OpCode::MOV;
                                    useCounts[memStateReg[base][offset]]++;
                                    inst.src1 = memStateReg[base][offset];
                                    inst.src2 = -1;
                                    inst.imm = 0;
                                    changed = true;
                                    lastStoreIdx[base].erase(offset);
                                }
                                else if (canForward && memStateImm[base].count(offset)) {
                                    inst.op = OpCode::CONST;
                                    inst.imm = memStateImm[base][offset];
                                    inst.src1 = -1;
                                    inst.src2 = -1;
                                    changed = true;
                                    lastStoreIdx[base].erase(offset);
                                }
                                else {
                                    if (knownAlias) {
                                        auto& sizeMap = memStateSize[base];
                                        auto& storeMap = lastStoreIdx[base];

                                        std::vector<uint64_t> toProtect;
                                        for (auto it = sizeMap.begin(); it != sizeMap.end(); ++it) {
                                            uint64_t existingOffset = it->first;
                                            uint64_t existingSize = it->second;
                                            bool overlap = (offset < existingOffset + existingSize) && (offset + inst.bytes > existingOffset);
                                            if (overlap) {
                                                toProtect.push_back(existingOffset);
                                            }
                                        }

                                        for (uint64_t protOff : toProtect) {
                                            storeMap.erase(protOff);
                                        }

                                        memStateReg[base][offset] = inst.dest;
                                        memStateSize[base][offset] = inst.bytes;
                                    }
                                    else {
                                        lastStoreIdx.clear();
                                    }
                                }
                            }

                            if (inst.dest != -1 && useCounts[inst.dest] == 0 && !hasSideEffects(inst)) {
                                constants.erase(inst.dest);
                                aliases.erase(inst.dest);
                                ptrBase.erase(inst.dest);
                                ptrOffset.erase(inst.dest);

                                for (auto it = availableExprs.begin(); it != availableExprs.end(); ) {
                                    if (it->second == inst.dest) {
                                        it = availableExprs.erase(it);
                                    }
                                    else {
                                        ++it;
                                    }
                                }

                                changed = true;
                                continue;
                            }

                            if (origSrc1 != -1) lastRegDefIdx.erase(origSrc1);
                            if (origSrc2 != -1) lastRegDefIdx.erase(origSrc2);
                            for (int arg : origArgs) lastRegDefIdx.erase(arg);

                            if (inst.dest != -1) {
                                if (lastRegDefIdx.count(inst.dest)) {
                                    int deadIdx = lastRegDefIdx[inst.dest];
                                    if (!deadInsts[deadIdx] && !hasSideEffects(newInsts[deadIdx])) {
                                        deadInsts[deadIdx] = true;
                                        changed = true;
                                    }
                                }
                                lastRegDefIdx[inst.dest] = newInsts.size();
                            }

                            if (inst.op == OpCode::STORE_LOCAL && localUses[inst.imm] == 0) {
                                changed = true;
                                continue;
                            }

                            newInsts.push_back(inst);
                            deadInsts.push_back(false);
                        }

                        std::vector<Instruction> finalInsts;
                        for (size_t i = 0; i < newInsts.size(); ++i) {
                            if (!deadInsts[i]) finalInsts.push_back(newInsts[i]);
                        }
                        block->instructions = std::move(finalInsts);
                    }
                    if (changed) fnPass2Changed = true;
                }
                if (fnPass2Changed) globalPassChanged = true;
            }












            bool sroaChanged = true;
            bool anySroaChanged = false;
            while (sroaChanged) {
                sroaChanged = false;
                for (auto& fn : mod.functions) {
                    std::map<int, bool> escapes;
                    std::map<int, int> rootBase;
                    std::map<int, int> rootOffset;

                    for (auto& block : fn.blocks) {
                        for (auto& inst : block->instructions) {
                            if (inst.op == OpCode::ALLOC && inst.dest != -1) {
                                escapes[inst.dest] = (inst.imm > 4096);
                                rootBase[inst.dest] = inst.dest;
                                rootOffset[inst.dest] = 0;
                            }
                            if (inst.op == OpCode::CALL && inst.label == "calloc" && inst.dest != -1) {
                                escapes[inst.dest] = true;
                                rootBase[inst.dest] = inst.dest;
                                rootOffset[inst.dest] = 0;
                            }
                        }
                    }

                    bool trackingChanged = true;
                    while (trackingChanged) {
                        trackingChanged = false;
                        for (auto& block : fn.blocks) {
                            for (auto& inst : block->instructions) {
                                if ((inst.op == OpCode::MOV || inst.op == OpCode::CAST) && inst.dest != -1) {
                                    if (rootBase.count(inst.src1) && !rootBase.count(inst.dest)) {
                                        rootBase[inst.dest] = rootBase[inst.src1];
                                        rootOffset[inst.dest] = rootOffset[inst.src1];
                                        trackingChanged = true;
                                    }
                                }

                                if (inst.op == OpCode::ADD && inst.dest != -1 && inst.src2 == -1) {
                                    if (rootBase.count(inst.src1) && !rootBase.count(inst.dest)) {
                                        rootBase[inst.dest] = rootBase[inst.src1];
                                        rootOffset[inst.dest] = rootOffset[inst.src1] + static_cast<int>(inst.imm);
                                        trackingChanged = true;
                                    }
                                }
                            }
                        }
                    }

                    auto markEscape = [&](int reg) {
                        if (reg != -1 && rootBase.count(reg)) escapes[rootBase[reg]] = true;
                    };

                    std::map<int, std::vector<std::pair<int, int>>> sroaAccesses;
                    std::map<int, std::set<int>> sroaBlocks;

                    for (auto& block : fn.blocks) {
                        for (auto& inst : block->instructions) {
                            if (inst.op == OpCode::ADD && inst.src2 != -1) {
                                markEscape(inst.src1);
                                markEscape(inst.src2);
                            }
                            if (inst.op == OpCode::CALL || inst.op == OpCode::STORE_LOCAL || inst.op == OpCode::RET || inst.op == OpCode::GET_PARAM) {
                                markEscape(inst.src1);
                                markEscape(inst.src2);
                                if (inst.op == OpCode::CALL || inst.op == OpCode::RET) markEscape(inst.dest);
                                if (inst.op == OpCode::CALL) {
                                    for (int arg : inst.args) markEscape(arg);
                                }
                            }
                            if (inst.op == OpCode::STORE) {
                                int ptrReg = -1;
                                int valReg = -1;

                                if (inst.isVolatile) {
                                    markEscape(inst.src1); markEscape(inst.src2); markEscape(inst.dest);
                                    continue;
                                }

                                if (rootBase.count(inst.dest)) { ptrReg = inst.dest; valReg = inst.src1; }
                                else if (inst.src1 != -1 && rootBase.count(inst.src1)) { ptrReg = inst.src1; valReg = inst.src2 != -1 ? inst.src2 : inst.dest; }
                                else if (inst.src2 != -1 && rootBase.count(inst.src2)) { ptrReg = inst.src2; valReg = inst.src1; }

                                if (ptrReg != -1) {
                                    int base = rootBase[ptrReg];
                                    sroaAccesses[base].push_back({ rootOffset[ptrReg], inst.bytes });
                                    sroaBlocks[base].insert(block->id);

                                    if (valReg != -1 && rootBase.count(valReg)) {
                                        markEscape(valReg);
                                    }
                                }
                                else {
                                    markEscape(inst.src1);
                                    markEscape(inst.src2);
                                    markEscape(inst.dest);
                                }
                            }
                            if (inst.op == OpCode::LOAD) {
                                int ptrReg = inst.src1;

                                if (inst.isVolatile) {
                                    markEscape(inst.src1);
                                    continue;
                                }

                                if (ptrReg != -1 && rootBase.count(ptrReg)) {
                                    int base = rootBase[ptrReg];
                                    sroaAccesses[base].push_back({ rootOffset[ptrReg], inst.bytes });
                                    sroaBlocks[base].insert(block->id);
                                }
                            }
                        }
                    }

                    for (auto& kv : sroaAccesses) {
                        int base = kv.first;
                        bool overlap = false;
                        for (size_t i = 0; i < kv.second.size(); i++) {
                            for (size_t j = i + 1; j < kv.second.size(); j++) {
                                int o1 = kv.second[i].first, s1 = kv.second[i].second;
                                int o2 = kv.second[j].first, s2 = kv.second[j].second;
                                if (o1 < o2 + s2 && o1 + s1 > o2) {
                                    if (o1 != o2 || s1 != s2) overlap = true;
                                }
                            }
                        }
                        if (overlap) escapes[base] = true;
                    }

                    /*for (auto& kv : sroaBlocks) {
                        if (kv.second.size() > 1) {
                            escapes[kv.first] = true;
                        }
                    }*/

                    std::map<int, std::map<int, int>> sroaRegs;
                    auto getSroaReg = [&](int base, int offset) {
                        if (!sroaRegs[base].count(offset)) sroaRegs[base][offset] = fn.allocVReg();
                        return sroaRegs[base][offset];
                    };

                    for (auto& block : fn.blocks) {
                        std::vector<Instruction> newInsts;
                        for (auto& inst : block->instructions) {
                            if (inst.op == OpCode::ALLOC && escapes.count(inst.dest) && !escapes[inst.dest]) {
                                sroaChanged = true; continue;
                            }
                            if (inst.op == OpCode::CALL && inst.label == "calloc" && escapes.count(inst.dest) && !escapes[inst.dest]) {
                                sroaChanged = true; continue;
                            }

                            if (inst.op == OpCode::STORE) {
                                int ptrReg = -1;
                                int valReg = -1;

                                if (rootBase.count(inst.dest)) { ptrReg = inst.dest; valReg = inst.src1; }
                                else if (inst.src1 != -1 && rootBase.count(inst.src1)) { ptrReg = inst.src1; valReg = inst.src2 != -1 ? inst.src2 : inst.dest; }
                                else if (inst.src2 != -1 && rootBase.count(inst.src2)) { ptrReg = inst.src2; valReg = inst.src1; }

                                if (ptrReg != -1) {
                                    int base = rootBase[ptrReg];
                                    if (!escapes[base]) {

                                        int offset = rootOffset[ptrReg];
                                        int staticReg = getSroaReg(base, offset);

                                        if (valReg != -1) {
                                            newInsts.push_back({ OpCode::MOV, staticReg, valReg, -1, 0, inst.bytes });
                                        }
                                        else {
                                            newInsts.push_back({ OpCode::CONST, staticReg, -1, -1, inst.imm, inst.bytes });
                                        }

                                        sroaChanged = true;
                                        anySroaChanged = true;
                                        continue;
                                    }
                                }
                            }

                            if (inst.op == OpCode::LOAD) {
                                int ptrReg = inst.src1;
                                if (ptrReg != -1 && rootBase.count(ptrReg)) {
                                    int base = rootBase[ptrReg];
                                    if (!escapes[base]) {
                                        int offset = rootOffset[ptrReg];
                                        int reg = getSroaReg(base, offset);
                                        newInsts.push_back({ OpCode::MOV, inst.dest, reg, -1, 0, inst.bytes });

                                        sroaChanged = true;
                                        anySroaChanged = true;
                                        continue;
                                    }
                                }
                            }
                            newInsts.push_back(inst);
                        }
                        block->instructions = std::move(newInsts);
                    }
                }
            }
            if (anySroaChanged) globalPassChanged = true;













            bool cfgChanged = true;
            bool anyCfgChanged = false;
            while (cfgChanged) {
                cfgChanged = false;
                for (auto& fn : mod.functions) {

                    std::unordered_set<int> reachable;
                    std::vector<int> worklist;
                    if (!fn.blocks.empty()) {
                        reachable.insert(fn.blocks[0]->id);
                        worklist.push_back(fn.blocks[0]->id);
                    }
                    while (!worklist.empty()) {
                        int curr = worklist.back();
                        worklist.pop_back();
                        BasicBlock* currentBlock = nullptr;
                        for (auto& b : fn.blocks) {
                            if (b->id == curr) { currentBlock = b.get(); break; }
                        }
                        if (currentBlock) {
                            for (auto& inst : currentBlock->instructions) {
                                if (inst.op == OpCode::JMP || inst.op == OpCode::JMP_FALSE) {
                                    if (reachable.insert(static_cast<int>(inst.imm)).second) {
                                        worklist.push_back(static_cast<int>(inst.imm));
                                    }
                                }
                            }
                        }
                    }

                    if (reachable.size() < fn.blocks.size()) {
                        std::vector<std::unique_ptr<BasicBlock>> reachableBlocks;
                        for (auto& b : fn.blocks) {
                            if (reachable.count(b->id)) {
                                reachableBlocks.push_back(std::move(b));
                            }
                        }
                        fn.blocks = std::move(reachableBlocks);
                        cfgChanged = true;
                        anyCfgChanged = true;
                    }

                    for (auto& b : fn.blocks) {
                        for (size_t k = 0; k < b->instructions.size(); ++k) {
                            if (b->instructions[k].op == OpCode::JMP || b->instructions[k].op == OpCode::RET) {
                                if (b->instructions.size() > k + 1) {
                                    b->instructions.resize(k + 1);
                                    cfgChanged = true;
                                    anyCfgChanged = true;
                                }
                                break;
                            }
                        }
                    }

                    for (size_t i = 0; i < fn.blocks.size(); ++i) {
                        auto& b = fn.blocks[i];
                        if (b->instructions.size() == 1 && b->instructions[0].op == OpCode::JMP) {
                            int targetId = b->instructions[0].imm;
                            if (targetId == b->id) continue;

                            for (auto& b2 : fn.blocks) {
                                for (auto& inst : b2->instructions) {
                                    if ((inst.op == OpCode::JMP || inst.op == OpCode::JMP_FALSE) && inst.imm == b->id) {
                                        inst.imm = targetId;
                                        cfgChanged = true;
                                        anyCfgChanged = true;
                                    }
                                }
                            }
                        }
                    }

                    std::map<int, int> inDegree;
                    for (auto& b : fn.blocks) {
                        for (auto& inst : b->instructions) {
                            if (inst.op == OpCode::JMP || inst.op == OpCode::JMP_FALSE) {
                                inDegree[inst.imm]++;
                            }
                        }
                    }
                    if (!fn.blocks.empty()) inDegree[fn.blocks[0]->id]++;

                    for (size_t i = 0; i < fn.blocks.size(); ++i) {
                        auto& b = fn.blocks[i];
                        if (b->instructions.empty()) continue;

                        auto& lastInst = b->instructions.back();
                        if (lastInst.op == OpCode::JMP) {
                            int targetId = lastInst.imm;
                            if (targetId != b->id && inDegree[targetId] == 1) {
                                BasicBlock* targetBlock = nullptr;
                                for (auto& tb : fn.blocks) {
                                    if (tb->id == targetId) { targetBlock = tb.get(); break; }
                                }

                                if (targetBlock && targetBlock != b.get()) {
                                    b->instructions.pop_back();
                                    b->instructions.insert(b->instructions.end(),
                                        targetBlock->instructions.begin(),
                                        targetBlock->instructions.end());
                                    targetBlock->instructions.clear();

                                    cfgChanged = true;
                                    anyCfgChanged = true;
                                }
                            }
                        }
                    }

                    if (cfgChanged) {
                        std::vector<std::unique_ptr<BasicBlock>> newBlocks;
                        int entryId = fn.blocks.empty() ? -1 : fn.blocks[0]->id;

                        for (auto& b : fn.blocks) {
                            if (!b->instructions.empty() || b->id == entryId) {
                                newBlocks.push_back(std::move(b));
                            }
                        }
                        fn.blocks = std::move(newBlocks);
                    }
                }
            }
            if (anyCfgChanged) globalPassChanged = true;












            bool licmChanged = true;
            while (licmChanged) {
                licmChanged = false;
                for (auto& fn : mod.functions) {
                    if (fn.blocks.size() <= 1) continue;
                    BasicBlock* entryBlock = fn.blocks[0].get();

                    std::unordered_set<int> entryDefs;
                    for (auto& inst : entryBlock->instructions) {
                        if (inst.dest != -1) entryDefs.insert(inst.dest);
                    }

                    std::unordered_set<int> mutatedRegs;
                    for (size_t bIdx = 1; bIdx < fn.blocks.size(); ++bIdx) {
                        for (auto& inst : fn.blocks[bIdx]->instructions) {
                            if (inst.dest != -1) mutatedRegs.insert(inst.dest);
                        }
                    }

                    for (size_t bIdx = 1; bIdx < fn.blocks.size(); ++bIdx) {
                        auto& block = fn.blocks[bIdx];
                        std::vector<Instruction> keptInsts;

                        for (auto& inst : block->instructions) {
                            bool isPure = (inst.op == OpCode::CONST || inst.op == OpCode::ADD ||
                                inst.op == OpCode::SUB || inst.op == OpCode::MUL ||
                                inst.op == OpCode::CAST || inst.op == OpCode::ZEXT ||
                                inst.op == OpCode::TRUNC || inst.op == OpCode::LOAD_STR ||
                                inst.op == OpCode::LEA_LOCAL);

                            if (isPure && inst.dest != -1) {
                                bool src1Safe = (inst.src1 == -1 || (entryDefs.count(inst.src1) && !mutatedRegs.count(inst.src1)));
                                bool src2Safe = (inst.src2 == -1 || (entryDefs.count(inst.src2) && !mutatedRegs.count(inst.src2)));

                                if (src1Safe && src2Safe && !mutatedRegs.count(inst.dest)) {
                                    auto insertIt = entryBlock->instructions.end();
                                    if (!entryBlock->instructions.empty()) {
                                        auto lastOp = entryBlock->instructions.back().op;
                                        if (lastOp == OpCode::JMP || lastOp == OpCode::JMP_FALSE || lastOp == OpCode::RET) {
                                            insertIt--;
                                        }
                                    }
                                    entryBlock->instructions.insert(insertIt, inst);
                                    entryDefs.insert(inst.dest);
                                    licmChanged = true;
                                    continue;
                                }
                            }
                            keptInsts.push_back(inst);
                        }
                        block->instructions = std::move(keptInsts);
                    }
                }
            }
        }
    }
}