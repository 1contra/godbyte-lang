#include "../include/irgen.hpp"
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <algorithm>

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
            uint64_t initVal = 0;
            if (v->initializer) {
                if (auto lit = dynamic_cast<const IntLiteral*>(v->initializer.get())) {
                    std::string txt = lit->value;
                    int base = (txt.starts_with("0x") || txt.starts_with("0X")) ? 16 : 10;
                    initVal = std::stoull(txt, nullptr, base);
                }
            }
            int size = v->resolvedType ? v->resolvedType->sizeBytes : 8;
            if (v->resolvedType && v->resolvedType->scalar == ScalarType::Struct && size == 0) {
                std::string sName = v->resolvedType->name;
                if (m_structMap.count(sName)) {
                    int maxSz = 0;
                    for (auto& f : m_structMap[sName]->fields) maxSz = std::max(maxSz, f.offset + 8);
                    size = maxSz;
                }
                else size = 128;
            }
            m_module.globals.push_back({ v->name, initVal, size });
        }

        for (const auto& st : prog.structs) {
            if (st->genericParams.empty() || st->name.find('$') != std::string::npos) {
                m_structMap[st->name] = st.get();
            }
        }

        for (const auto& fn : prog.functions) {
            if (fn->genericParams.empty() || fn->name.find('$') != std::string::npos) {
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
        if (!fn.name.starts_with(".sec$")) {
            for (const auto& attr : fn.attributes) {
                if (attr.kind == AttrKind::Section && !attr.args.empty()) {
                    irFn.name = ".sec$" + attr.args[0] + "$" + fn.name;
                    break;
                }
            }
        }

        irFn.argCount = (int)fn.params.size();

        if (hasAttribute(fn.attributes, AttrKind::Inline)) irFn.isInline = true;
        if (hasAttribute(fn.attributes, AttrKind::Export)) irFn.isExported = true;

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
            /*
            if (hasAttribute(fn.attributes, AttrKind::Inline)) {
                emit({ OpCode::MOV, vReg, -1, -1, 0, size });
            }
            else {
                emit({ OpCode::GET_PARAM, vReg, -1, -1, (uint64_t)i, size });
            }*/
        }

        if (hasAttribute(fn.attributes, AttrKind::Inline) && !m_currentBlock->instructions.empty() &&
            m_currentBlock->instructions.back().op == OpCode::JMP) {
            m_currentBlock->instructions.pop_back();
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

        if (hasAttribute(fn.attributes, AttrKind::Inline) && !m_currentBlock->instructions.empty() &&
            m_currentBlock->instructions.back().op == OpCode::RET) {
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
            bool isStackPrimitive = hasAttribute(decl->attributes, AttrKind::Addr) && !isStruct && !isArray;

            if (isStruct && size == 0) {
                std::string sName = decl->resolvedType->name;
                if (m_structMap.count(sName)) {
                    int maxSz = 0;
                    for (auto& f : m_structMap[sName]->fields) maxSz = std::max(maxSz, f.offset + 8);
                    size = maxSz;
                }
                else {
                    size = 128;
                }
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
            if (m_currentBlock && (m_currentBlock->instructions.empty() ||
                (m_currentBlock->instructions.back().op != OpCode::JMP &&
                    m_currentBlock->instructions.back().op != OpCode::RET))) {
                emit({ OpCode::JMP, -1, -1, -1, (uint64_t)endBlock->id });
            }

            if (elseBlock) {
                m_currentBlock = elseBlock;
                genStmt(*ifStmt->elseBranch);
                if (m_currentBlock && (m_currentBlock->instructions.empty() ||
                    (m_currentBlock->instructions.back().op != OpCode::JMP &&
                        m_currentBlock->instructions.back().op != OpCode::RET))) {
                    emit({ OpCode::JMP, -1, -1, -1, (uint64_t)endBlock->id });
                }
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
            m_loopUpdates.push_back(condBlock);

            m_currentBlock = bodyBlock;
            genStmt(*whileStmt->body);

            m_loopExits.pop_back();
            m_loopUpdates.pop_back();

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
            m_loopUpdates.push_back(updateBlock);

            m_currentBlock = bodyBlock;
            if (forStmt->body) {
                genStmt(*forStmt->body);
            }

            m_loopExits.pop_back();
            m_loopUpdates.pop_back();

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

        /*
        else if (auto whileStmt = dynamic_cast<const WhileStmt*>(&stmt)) {
            BasicBlock* condBlock = m_currentFunc->createBlock();
            BasicBlock* bodyBlock = m_currentFunc->createBlock();
            BasicBlock* updateBlock = m_currentFunc->createBlock();
            BasicBlock* endBlock = m_currentFunc->createBlock();

            emit({ OpCode::JMP, -1, -1, -1, (uint64_t)condBlock->id });
            m_currentBlock = condBlock;

            int cond = genExpr(*whileStmt->condition);
            emit({ OpCode::JMP_FALSE, -1, cond, -1, (uint64_t)endBlock->id });
            emit({ OpCode::JMP, -1, -1, -1, (uint64_t)bodyBlock->id });

            m_loopExits.push_back(endBlock);
            m_loopUpdates.push_back(updateBlock);

            m_currentBlock = bodyBlock;
            genStmt(*whileStmt->body);

            m_loopExits.pop_back();
            m_loopUpdates.pop_back();

            if (m_currentBlock) {
                emit({ OpCode::JMP, -1, -1, -1, (uint64_t)updateBlock->id });
            }

            m_currentBlock = updateBlock;
            int loopCond = genExpr(*whileStmt->condition);
            emit({ OpCode::JMP_FALSE, -1, loopCond, -1, (uint64_t)endBlock->id });
            emit({ OpCode::JMP, -1, -1, -1, (uint64_t)bodyBlock->id });

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
            m_loopUpdates.push_back(updateBlock);

            m_currentBlock = bodyBlock;
            if (forStmt->body) {
                genStmt(*forStmt->body);
            }

            m_loopExits.pop_back();
            m_loopUpdates.pop_back();

            if (m_currentBlock) {
                emit({ OpCode::JMP, -1, -1, -1, (uint64_t)updateBlock->id });
            }
            m_currentBlock = updateBlock;
            if (forStmt->update) {
                genExpr(*forStmt->update);
            }
            if (forStmt->condition) {
                int cond = genExpr(*forStmt->condition);
                emit({ OpCode::JMP_FALSE, -1, cond, -1, (uint64_t)endBlock->id });
                emit({ OpCode::JMP, -1, -1, -1, (uint64_t)bodyBlock->id });
            }
            else {
                emit({ OpCode::JMP, -1, -1, -1, (uint64_t)bodyBlock->id });
            }
            m_currentBlock = endBlock;
        }
        */
        else if (auto breakStmt = dynamic_cast<const BreakStmt*>(&stmt)) {
            if (!m_loopExits.empty()) {
                emit({ OpCode::JMP, -1, -1, -1, (uint64_t)m_loopExits.back()->id });
                m_currentBlock = nullptr;
            }
        }
        else if (auto cIf = dynamic_cast<const ComptimeIfStmt*>(&stmt)) {
            if (cIf->thenBranch) genStmt(*cIf->thenBranch);
            if (cIf->elseBranch) genStmt(*cIf->elseBranch);
        }
        else if (auto contStmt = dynamic_cast<const ContinueStmt*>(&stmt)) {
            if (!m_loopUpdates.empty()) {
                emit({ OpCode::JMP, -1, -1, -1, (uint64_t)m_loopUpdates.back()->id });
                m_currentBlock = nullptr;
            }
        }
    }

    int IRGenerator::genExpr(const Expr& expr) {
        if (expr.type && expr.type->scalar == ScalarType::FunctionPtr) {
            
        }
        else if (auto lit = dynamic_cast<const IntLiteral*>(&expr)) {
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
            int size = sInit->type ? sInit->type->sizeBytes : 8;

            if (size == 0) {
                std::string sName = sInit->type->name;
                if (m_structMap.count(sName)) {
                    int maxSz = 0;
                    for (auto& f : m_structMap[sName]->fields) maxSz = std::max(maxSz, f.offset + 8);
                    size = maxSz;
                }
                else size = 128;
            }

            emit({ OpCode::ALLOC, dest, -1, -1, (uint64_t)size });

            for (const auto& fInit : sInit->fields) {
                int valReg = genExpr(*fInit.value);
                int offset = getOffset(sInit->type, fInit.name);
                int addrReg = m_currentFunc->allocVReg();

                if (offset >= 0) {
                    emit({ OpCode::ADD, addrReg, dest, -1, (uint64_t)offset, 8 });
                }
                else {
                    emit({ OpCode::MOV, addrReg, dest, -1, 0, 8 });
                }

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
                int addrReg = m_currentFunc->allocVReg();
                Instruction inst;
                inst.op = OpCode::LOAD_STR;
                inst.dest = addrReg;
                inst.label = var->name;
                inst.bytes = 8;
                emit(inst);

                if (var->type && !var->type->isArray && var->type->scalar != ScalarType::Struct && var->type->scalar != ScalarType::FunctionPtr) {
                    Instruction loadInst = { OpCode::LOAD, res, addrReg, -1, 0, size };
                    if (var->type && var->type->isVolatile) loadInst.isVolatile = true;
                    emit(loadInst);
                }
                else {
                    emit({ OpCode::MOV, res, addrReg, -1, 0, 8 });
                }
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
            if (arr->overloadedCall) {
                int callRes = genExpr(*arr->overloadedCall);
                Type* callRetType = arr->overloadedCall->type;
                if (callRetType && callRetType->isPointer()) {
                    return callRes;
                }
                else {
                    std::cerr << "[IRGen Error] Cannot take address of overloaded operator[] that returns by value.\n";
                    exit(1);
                }
            }
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
        else if (auto builtin = dynamic_cast<const BuiltinCallExpr*>(&expr)) {
            if (builtin->builtinType == TokenType::BuiltinMemfill) {
                int destReg = genExpr(*builtin->args[0]);
                int valReg = genExpr(*builtin->args[1]);
                int sizeReg = genExpr(*builtin->args[2]);
                Instruction inst;
                inst.op = OpCode::CALL;
                inst.dest = m_currentFunc->allocVReg();
                inst.label = "memset";
                inst.args = { destReg, valReg, sizeReg };
                inst.argBytes = { 8, 8, 8 };
                emit(inst);
                return inst.dest;
            }
            else if (builtin->builtinType == TokenType::BuiltinAllocate) {
                if (builtin->args.size() != 2) {
                    throw std::runtime_error("allocate expects exactly 2 arguments");
                }

                int sizeReg = genExpr(*builtin->args[0]);
                int oneReg = m_currentFunc->allocVReg();
                emit({ OpCode::CONST, oneReg, -1, -1, 1, 8 });

                int dest = m_currentFunc->allocVReg();

                Instruction inst;
                inst.op = OpCode::CALL;
                inst.dest = dest;
                inst.label = "calloc";
                inst.args = { oneReg, sizeReg };
                inst.argBytes = { 8, 8 };
                emit(inst);

                return dest;
            }
            else if (builtin->builtinType == TokenType::BuiltinMemcpy) {
                int destReg = genExpr(*builtin->args[0]);
                int srcReg = genExpr(*builtin->args[1]);

                if (auto sizeLit = dynamic_cast<const IntLiteral*>(builtin->args[2].get())) {
                    uint64_t size = 0;
                    try {
                        size = std::stoull(sizeLit->value);
                    }
                    catch (...) {}

                    if (size > 0 && size <= 8) {
                        int tempReg = m_currentFunc->allocVReg();
                        emit({ OpCode::LOAD, tempReg, srcReg, -1, 0, static_cast<int>(size) });
                        emit({ OpCode::STORE, -1, destReg, tempReg, 0, static_cast<int>(size) });
                        return destReg;
                    }
                }

                int sizeReg = genExpr(*builtin->args[2]);
                Instruction inst;
                inst.op = OpCode::CALL;
                inst.dest = m_currentFunc->allocVReg();
                inst.label = "memcpy";
                inst.args = { destReg, srcReg, sizeReg };
                inst.argBytes = { 8, 8, 8 };
                emit(inst);
                return inst.dest;
            }
            else if (builtin->builtinType == TokenType::BuiltinTrap) {
                emit({ OpCode::TRAP, -1, -1, -1, 0, 0, ScalarType::Void });
                return 0;
            }
            else if (builtin->builtinType == TokenType::BuiltinUnreachable) {
                emit({ OpCode::UNREACHABLE, -1, -1, -1, 0, 0, ScalarType::Void });
                return 0;
            }
            else if (builtin->builtinType == TokenType::BuiltinBswap) {
                int srcReg = genExpr(*builtin->args[0]);
                int dest = m_currentFunc->allocVReg();
                int size = builtin->type ? builtin->type->sizeBytes : 8;
                emit({ OpCode::BSWAP, dest, srcReg, -1, 0, size, builtin->type ? builtin->type->scalar : ScalarType::Void });
                return dest;
            }
        }
        else if (auto hasMeth = dynamic_cast<const CompilerHasMethodExpr*>(&expr)) {
            int d = m_currentFunc->allocVReg();
            emit({ OpCode::CONST, d, -1, -1, (uint64_t)hasMeth->resultValue, 1 });
            return d;
        }
        else if (auto al = dynamic_cast<const AlignofExpr*>(&expr)) {
            int d = m_currentFunc->allocVReg();
            emit({ OpCode::CONST, d, -1, -1, 8, 8 });
            return d;
        }
        else if (auto exp = dynamic_cast<const ExpandExpr*>(&expr)) {
            return genExpr(*exp->operand);
        }
        else if (auto sizeExpr = dynamic_cast<const SizeofExpr*>(&expr)) {
            int d = m_currentFunc->allocVReg();
            uint64_t structSize = sizeExpr->resolvedTargetType->sizeBytes;

            if (structSize == 0 && sizeExpr->resolvedTargetType->scalar == ScalarType::Struct) {
                std::string sName = sizeExpr->resolvedTargetType->name;
                if (m_structMap.count(sName)) {
                    int maxSz = 0;
                    for (auto& f : m_structMap[sName]->fields) maxSz = std::max(maxSz, f.offset + 8);
                    structSize = maxSz;
                }
            }

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
                if (assignArr->overloadedCall) {
                    int addr = genExpr(*assignArr->overloadedCall);
                    Type* callRetType = assignArr->overloadedCall->type;
                    if (!callRetType || !callRetType->isPointer()) {
                        std::cerr << "[IRGen Fatal Error] Overloaded operator[] must return a reference (ref T) to be assigned.\n";
                        exit(1);
                    }

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
                    if (m_locals.count(assignVar->name) && !m_stackPrimitives.count(assignVar->name)) {
                        emit({ OpCode::MOV, curr, m_locals[assignVar->name], -1, 0, storeSize });
                    }
                    else if (m_stackPrimitives.count(assignVar->name)) {
                        Instruction loadInst = { OpCode::LOAD, curr, m_locals[assignVar->name], -1, 0, storeSize };
                        if (assignVar->type && assignVar->type->isVolatile) loadInst.isVolatile = true;
                        emit(loadInst);
                    }
                    else {
                        int addrReg = m_currentFunc->allocVReg();
                        Instruction addrInst = { OpCode::LOAD_STR, addrReg, -1, -1, 0, 8 };
                        addrInst.label = assignVar->name;
                        emit(addrInst);

                        Instruction loadInst = { OpCode::LOAD, curr, addrReg, -1, 0, storeSize };
                        if (assignVar->type && assignVar->type->isVolatile) loadInst.isVolatile = true;
                        emit(loadInst);
                    }
                    finalVal = getFinalVal(curr);
                }

                if (m_locals.count(assignVar->name)) {
                    if (m_stackPrimitives.count(assignVar->name)) {
                        Instruction storeInst = { OpCode::STORE, -1, m_locals[assignVar->name], finalVal, 0, storeSize };
                        if (assignVar->type && assignVar->type->isVolatile) storeInst.isVolatile = true;
                        emit(storeInst);
                    }
                    else {
                        emit({ OpCode::MOV, m_locals[assignVar->name], finalVal, -1, 0, storeSize });
                    }
                }
                else {
                    int addrReg = m_currentFunc->allocVReg();
                    Instruction addrInst = { OpCode::LOAD_STR, addrReg, -1, -1, 0, 8 };
                    addrInst.label = assignVar->name;
                    emit(addrInst);

                    Instruction storeInst = { OpCode::STORE, -1, addrReg, finalVal, 0, storeSize };
                    if (assignVar->type && assignVar->type->isVolatile) storeInst.isVolatile = true;
                    emit(storeInst);
                }
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
            std::cerr << "[IRGen Fatal Error] Unsupported assignment target lvalue type encountered!\n";
            exit(1);
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
                    std::cerr << "[IRGen Error] Cannot take address of primitive... Use [[@addr]] attribute.\n";
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
            else if (auto arr = dynamic_cast<const ArrayAccessExpr*>(target)) {\
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
            if (bin->overloadedCall) {
                return genExpr(*bin->overloadedCall);
            }

            if (bin->op == TokenType::AmpAmp || bin->op == TokenType::PipePipe) {
                int d = m_currentFunc->allocVReg();
                int size = bin->type ? bin->type->sizeBytes : 8;
                BasicBlock* rightBlock = m_currentFunc->createBlock();
                BasicBlock* endBlock = m_currentFunc->createBlock();

                int l = genExpr(*bin->left);
                emit({ OpCode::MOV, d, l, -1, 0, size });

                int zeroReg = m_currentFunc->allocVReg();
                emit({ OpCode::CONST, zeroReg, -1, -1, 0, size });
                int isTrue = m_currentFunc->allocVReg();
                emit({ OpCode::CMP_NE, isTrue, l, zeroReg, 0, size });

                if (bin->op == TokenType::AmpAmp) {
                    emit({ OpCode::JMP_FALSE, -1, isTrue, -1, (uint64_t)endBlock->id });
                }
                else {
                    BasicBlock* nextCheck = m_currentFunc->createBlock();
                    emit({ OpCode::JMP_FALSE, -1, isTrue, -1, (uint64_t)nextCheck->id });
                    emit({ OpCode::JMP, -1, -1, -1, (uint64_t)endBlock->id });
                    m_currentBlock = nextCheck;
                }

                emit({ OpCode::JMP, -1, -1, -1, (uint64_t)rightBlock->id });
                m_currentBlock = rightBlock;
                int r = genExpr(*bin->right);
                emit({ OpCode::MOV, d, r, -1, 0, size });
                emit({ OpCode::JMP, -1, -1, -1, (uint64_t)endBlock->id });

                m_currentBlock = endBlock;
                return d;
            }

            int l = genExpr(*bin->left);
            int r = genExpr(*bin->right);
            int d = m_currentFunc->allocVReg();
            bool isFloat = bin->type && bin->type->isFloatingPoint();
            int size = bin->type ? bin->type->sizeBytes : 8;
            OpCode op = OpCode::ADD;

            switch (bin->op) {
                case TokenType::Plus: op = isFloat ? OpCode::FADD : OpCode::ADD; break;
                case TokenType::Minus: op = isFloat ? OpCode::FSUB : OpCode::SUB; break;
                case TokenType::Star: op = isFloat ? OpCode::FMUL : OpCode::MUL; break;
                case TokenType::Slash:
                    if (isFloat) op = OpCode::FDIV;
                    else op = (bin->type && bin->type->isSigned) ? OpCode::DIV : OpCode::UDIV;
                    break;
                case TokenType::Percent:
                    op = (bin->type && bin->type->isSigned) ? OpCode::MOD : OpCode::UMOD;
                    break;
                case TokenType::LT: op = OpCode::CMP_LT; break;
                case TokenType::GT: op = OpCode::CMP_GT; break;
                case TokenType::LE: op = OpCode::CMP_LE; break;
                case TokenType::GE: op = OpCode::CMP_GE; break;
                case TokenType::EqualEqual: op = OpCode::CMP_EQ; break;
                case TokenType::NotEqual: op = OpCode::CMP_NE; break;
                case TokenType::Pipe: op = OpCode::OR; break;
                case TokenType::Ampersand: op = OpCode::AND; break;
                case TokenType::Caret: op = OpCode::XOR; break;
                case TokenType::ShiftLeft: op = OpCode::SHL; break;
                case TokenType::ShiftRight: op = OpCode::SHR; break;
                default: break;
            }
            emit({ op, d, l, r, 0, static_cast<int>(size) });
            return d;
        }

        if (auto call = dynamic_cast<const CallExpr*>(&expr)) {
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

            bool isInlineFunctionCall = false;
            if (auto var = dynamic_cast<const VarExpr*>(call->callee.get())) {
                for (const auto& fn : m_module.functions) {
                    if (fn.name == var->name && fn.isInline) {
                        isInlineFunctionCall = true;
                        break;
                    }
                }
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

            if (isInlineFunctionCall && dest != -1) {
                emit(inst);
                return dest;
            }
            else {
                emit(inst);

                if (isVoid) {
                    int dummy = m_currentFunc->allocVReg();
                    emit({ OpCode::CONST, dummy, -1, -1, 0, 8 });
                    return dummy;
                }
                return dest != -1 ? dest : 0;
            }
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
                if (destSize > srcSize) {
                    bool isSigned = cast->operand->type && cast->operand->type->isSigned;
                    inst.op = isSigned ? OpCode::SEXT : OpCode::ZEXT;
                }
                else {
                    inst.op = OpCode::TRUNC;
                }

                inst.dest = dest;
                inst.src1 = src;
                inst.bytes = destSize;
                inst.imm = srcSize;
                emit(inst);
            }
            return dest;
        }
        else if (auto lockExpr = dynamic_cast<const LockExpr*>(&expr)) {
            return genExpr(*lockExpr->operand);
        }
        std::cerr << "[IRGen Error] Unsupported expression type encountered: " << typeid(expr).name() << "\n";
        exit(1);
    }

    static bool vectorizeLoop(IRFunction& fn, BasicBlock* loopBlock, int& seqVecReg) {
        if (loopBlock->name.find("_vec") != std::string::npos) return false;

        for (auto& inst : loopBlock->instructions) {
            if (inst.op == OpCode::DIV || inst.op == OpCode::FDIV ||
                inst.op == OpCode::CALL || inst.op == OpCode::RET ||
                inst.op == OpCode::INLINE_ASM || inst.op == OpCode::ALLOC || inst.op == OpCode::CMP_EQ) {
                return false;
            }
        }

        int indReg = -1;
        for (auto& inst : loopBlock->instructions) {
            if ((inst.op == OpCode::CMP_LT || inst.op == OpCode::CMP_LE) && inst.src2 == -1) {
                indReg = inst.src1;
            }
        }
        if (indReg == -1) return false;

        int indIncr = -1;
        for (auto& inst : loopBlock->instructions) {
            if (inst.op == OpCode::ADD && (inst.dest == indReg || inst.src1 == indReg) && inst.imm == 1) {
                indIncr = 1;
                break;
            }
        }
        if (indIncr == -1) return false;

        std::set<int> addrRegs;
        std::vector<int> worklist;
        for (auto& inst : loopBlock->instructions) {
            if (inst.op == OpCode::LOAD || inst.op == OpCode::STORE) {
                if (inst.src1 != -1) worklist.push_back(inst.src1);
            }
        }
        while (!worklist.empty()) {
            int r = worklist.back(); worklist.pop_back();
            if (addrRegs.count(r)) continue;
            addrRegs.insert(r);
            for (auto& inst : loopBlock->instructions) {
                if (inst.dest == r) {
                    if (inst.src1 != -1) worklist.push_back(inst.src1);
                    if (inst.src2 != -1) worklist.push_back(inst.src2);
                }
            }
        }

        bool hasPayload = false;
        for (auto& inst : loopBlock->instructions) {
            if (inst.op == OpCode::STORE) hasPayload = true;
        }
        if (!hasPayload) return false;

        std::vector<Instruction> vecInsts;
        std::map<int, int> vecRegMap;

        if (seqVecReg == -1) {
            int seqAlloc = fn.allocVReg();
            std::vector<Instruction> seqInsts;
            seqInsts.push_back({ OpCode::ALLOC, seqAlloc, -1, -1, 32, 8 });
            for (uint64_t i = 0; i < 4; ++i) {
                int valReg = fn.allocVReg();
                seqInsts.push_back({ OpCode::CONST, valReg, -1, -1, i, 8 });
                int ptrReg = fn.allocVReg();
                seqInsts.push_back({ OpCode::ADD, ptrReg, seqAlloc, -1, i * 8, 8 });
                Instruction st = { OpCode::STORE, -1, ptrReg, valReg, 0, 8 };
                st.isVolatile = true;
                seqInsts.push_back(st);
            }
            seqVecReg = fn.allocVReg();
            seqInsts.push_back({ OpCode::VLOAD256, seqVecReg, seqAlloc, -1, 0, 32 });

            auto insertIt = fn.blocks[0]->instructions.end();
            while (insertIt != fn.blocks[0]->instructions.begin()) {
                auto prevIt = std::prev(insertIt);
                if (prevIt->op == OpCode::JMP || prevIt->op == OpCode::JMP_FALSE || prevIt->op == OpCode::RET) {
                    insertIt = prevIt;
                }
                else {
                    break;
                }
            }
            fn.blocks[0]->instructions.insert(insertIt, seqInsts.begin(), seqInsts.end());
        }

        int vecIndReg = -1;
        auto getVecIndReg = [&]() {
            if (vecIndReg != -1) return vecIndReg;
            int bcast = fn.allocVReg();
            vecInsts.push_back({ OpCode::VPBROADCASTQ, bcast, indReg, -1, 0, 32 });
            vecIndReg = fn.allocVReg();
            vecInsts.push_back({ OpCode::VADD256, vecIndReg, bcast, seqVecReg, 0, 32 });
            return vecIndReg;
        };

        auto handlePayloadOperand = [&](int src, uint64_t imm) {
            if (src == -1) {
                int cReg = fn.allocVReg();
                int bReg = fn.allocVReg();

                auto insertIt = fn.blocks[0]->instructions.end();
                while (insertIt != fn.blocks[0]->instructions.begin()) {
                    auto prevIt = std::prev(insertIt);
                    if (prevIt->op == OpCode::JMP || prevIt->op == OpCode::JMP_FALSE || prevIt->op == OpCode::RET) {
                        insertIt = prevIt;
                    }
                    else {
                        break;
                    }
                }
                insertIt = fn.blocks[0]->instructions.insert(insertIt, { OpCode::CONST, cReg, -1, -1, imm, 8 });
                insertIt++;
                insertIt = fn.blocks[0]->instructions.insert(insertIt, { OpCode::VPBROADCASTQ, bReg, cReg, -1, 0, 32 });
                return bReg;
            }
            if (src == indReg) return getVecIndReg();
            if (vecRegMap.count(src)) return vecRegMap[src];

            int bReg = fn.allocVReg();
            vecInsts.push_back({ OpCode::VPBROADCASTQ, bReg, src, -1, 0, 32 });
            return bReg;
        };

        for (auto& inst : loopBlock->instructions) {
            if (inst.op == OpCode::CMP_LT || inst.op == OpCode::CMP_LE || inst.op == OpCode::JMP_FALSE || inst.op == OpCode::JMP || inst.op == OpCode::CONST || inst.op == OpCode::CAST || inst.op == OpCode::ZEXT || inst.op == OpCode::TRUNC) {
                vecInsts.push_back(inst);
                continue;
            }

            if (inst.op == OpCode::ADD && (inst.dest == indReg || inst.src1 == indReg) && inst.imm == 1) {
                Instruction i = inst;
                i.imm = 4;
                vecInsts.push_back(i);
                continue;
            }

            if (inst.op == OpCode::LOAD) {
                Instruction vInst = inst;
                vInst.op = OpCode::VLOAD256;
                vInst.bytes = 32;
                vInst.dest = fn.allocVReg();
                vecRegMap[inst.dest] = vInst.dest;
                vecInsts.push_back(vInst);
                continue;
            }

            if (inst.op == OpCode::STORE) {
                Instruction vInst = inst;
                vInst.op = OpCode::VSTORE256;
                vInst.bytes = 32;
                if (inst.src2 != -1) {
                    vInst.src2 = handlePayloadOperand(inst.src2, 0);
                }
                else {
                    vInst.src1 = inst.src1;
                    vInst.src2 = handlePayloadOperand(-1, inst.imm);
                    vInst.imm = 0;
                }
                vecInsts.push_back(vInst);
                continue;
            }

            if (inst.op == OpCode::ADD || inst.op == OpCode::SUB || inst.op == OpCode::MUL ||
                inst.op == OpCode::AND || inst.op == OpCode::OR || inst.op == OpCode::XOR ||
                inst.op == OpCode::SHL || inst.op == OpCode::SHR) {

                if (addrRegs.count(inst.dest)) {
                    vecInsts.push_back(inst);
                }
                else {
                    Instruction vInst = inst;

                    if (inst.op == OpCode::SHL && inst.src2 == -1) {
                        if (inst.imm == 1) {
                            vInst.op = OpCode::VADD256;
                            vInst.bytes = 32;
                            vInst.dest = fn.allocVReg();
                            vecRegMap[inst.dest] = vInst.dest;

                            int vectorSrc = handlePayloadOperand(inst.src1, 0);
                            vInst.src1 = vectorSrc;
                            vInst.src2 = vectorSrc;
                            vInst.imm = 0;
                            vecInsts.push_back(vInst);
                            continue;
                        }
                        else {
                            vInst.op = OpCode::VMUL256;
                            vInst.bytes = 32;
                            vInst.dest = fn.allocVReg();
                            vecRegMap[inst.dest] = vInst.dest;
                            vInst.src1 = handlePayloadOperand(inst.src1, 0);
                            vInst.src2 = handlePayloadOperand(-1, 1ULL << inst.imm);
                            vInst.imm = 0;
                            vecInsts.push_back(vInst);
                            continue;
                        }
                    }

                    switch (inst.op) {
                        case OpCode::ADD: vInst.op = OpCode::VADD256; break;
                        case OpCode::SUB: vInst.op = OpCode::VSUB256; break;
                        case OpCode::MUL: vInst.op = OpCode::VMUL256; break;
                        case OpCode::AND: vInst.op = OpCode::VAND256; break;
                        case OpCode::OR:  vInst.op = OpCode::VOR256; break;
                        case OpCode::XOR: vInst.op = OpCode::VXOR256; break;
                        default: break;
                    }
                    vInst.bytes = 32;
                    vInst.dest = fn.allocVReg();
                    vecRegMap[inst.dest] = vInst.dest;

                    vInst.src1 = handlePayloadOperand(inst.src1, 0);
                    if (inst.src2 != -1) {
                        vInst.src2 = handlePayloadOperand(inst.src2, 0);
                    }
                    else {
                        vInst.src2 = handlePayloadOperand(-1, inst.imm);
                        vInst.imm = 0;
                    }
                    vecInsts.push_back(vInst);
                }
                continue;
            }
            vecInsts.push_back(inst);
        }
        loopBlock->instructions = std::move(vecInsts);
        loopBlock->name += "_vec";
        return true;
    }







    void IRGenerator::optimize(IRModule& mod, bool enableOpt) {
        if (!enableOpt) return;
















        bool ifConvChanged = true;
        while (ifConvChanged) {
            ifConvChanged = false;
            for (auto& fn : mod.functions) {
                for (size_t i = 0; i < fn.blocks.size(); ++i) {
                    auto& block = fn.blocks[i];
                    if (block->instructions.size() < 2) continue;

                    auto& lastInst = block->instructions.back();
                    auto& prevInst = block->instructions[block->instructions.size() - 2];

                    if (prevInst.op == OpCode::JMP_FALSE && lastInst.op == OpCode::JMP) {
                        int condReg = prevInst.src1;
                        int falseTarget = prevInst.imm;
                        int trueTarget = lastInst.imm;

                        BasicBlock* trueBlock = nullptr;
                        BasicBlock* falseBlock = nullptr;
                        for (auto& b : fn.blocks) {
                            if (b->id == trueTarget) trueBlock = b.get();
                            if (b->id == falseTarget) falseBlock = b.get();
                        }

                        auto isPureBlock = [](BasicBlock* b) {
                            if (!b || b->instructions.size() < 2) return false;
                            if (b->instructions.back().op != OpCode::JMP) return false;
                            for (size_t k = 0; k < b->instructions.size() - 1; ++k) {
                                auto& inst = b->instructions[k];
                                if (inst.isVolatile || inst.op == OpCode::CALL || inst.op == OpCode::STORE ||
                                    inst.op == OpCode::LOAD || inst.op == OpCode::STORE_LOCAL ||
                                    inst.op == OpCode::LOAD_LOCAL || inst.op == OpCode::ALLOC) return false;
                                if (inst.op == OpCode::JMP || inst.op == OpCode::JMP_FALSE || inst.op == OpCode::RET) return false;
                            }
                            return true;
                        };

                        if (isPureBlock(trueBlock) && isPureBlock(falseBlock)) {
                            int trueDest = trueBlock->instructions[trueBlock->instructions.size() - 2].dest;
                            int falseDest = falseBlock->instructions[falseBlock->instructions.size() - 2].dest;

                            int trueExit = trueBlock->instructions.back().imm;
                            int falseExit = falseBlock->instructions.back().imm;

                            if (trueDest != -1 && trueDest == falseDest && trueExit == falseExit) {
                                int size = trueBlock->instructions[trueBlock->instructions.size() - 2].bytes;
                                block->instructions.pop_back();
                                block->instructions.pop_back();

                                int trueDestReg = fn.allocVReg();
                                int falseDestReg = fn.allocVReg();

                                for (size_t k = 0; k < trueBlock->instructions.size() - 1; ++k) {
                                    Instruction inst = trueBlock->instructions[k];
                                    if (k == trueBlock->instructions.size() - 2) inst.dest = trueDestReg;
                                    block->instructions.push_back(inst);
                                }

                                for (size_t k = 0; k < falseBlock->instructions.size() - 1; ++k) {
                                    Instruction inst = falseBlock->instructions[k];
                                    if (k == falseBlock->instructions.size() - 2) inst.dest = falseDestReg;
                                    block->instructions.push_back(inst);
                                }

                                Instruction selInst = { OpCode::SELECT, trueDest, condReg, trueDestReg, 0, size };
                                selInst.args.push_back(falseDestReg);
                                block->instructions.push_back(selInst);
                                block->instructions.push_back({ OpCode::JMP, -1, -1, -1, (uint64_t)trueExit });

                                trueBlock->instructions.clear();
                                falseBlock->instructions.clear();

                                ifConvChanged = true;
                                break;
                            }
                        }
                    }
                }
            }
        }









        bool globalPassChanged = true;
        while (globalPassChanged) {
            globalPassChanged = false;


            std::cout << "[Optimizer] Starting pass loop..." << std::endl;


            std::cout << "[Optimizer] Running inliner" << std::endl;
            bool inlinedAnything = true;
            while (inlinedAnything) {
                inlinedAnything = false;
                for (auto& fn : mod.functions) {
                    for (size_t bIdx = 0; bIdx < fn.blocks.size(); ++bIdx) {
                        BasicBlock* currentBlock = fn.blocks[bIdx].get();
                        std::vector<Instruction> newInsts;
                        bool didInlineHere = false;
                        
                        for (size_t i = 0; i < currentBlock->instructions.size(); ++i) {
                            auto& inst = currentBlock->instructions[i];
                            if (inst.op == OpCode::CALL && !inst.label.empty()) {
                                IRFunction* target = nullptr;
                                for (auto& t : mod.functions) {
                                    if (t.name == inst.label) { target = &t; break; }
                                }

                                bool isMutuallyRecursive = false;
                                if (currentBlock->name.find("$inlined_" + inst.label + "$") != std::string::npos) {
                                    isMutuallyRecursive = true;
                                }

                                if (target && target != &fn && !isMutuallyRecursive && !target->blocks.empty() && (target->isInline || target->blocks.size() <= 4)) {
                                    int instCount = 0;
                                    for (auto& tblock : target->blocks) instCount += tblock->instructions.size();

                                    if (instCount <= 30) {
                                        std::map<int, int> vregMap;
                                        std::map<int, int> blockMap;

                                        for (auto& tblock : target->blocks) {
                                            BasicBlock* newB = fn.createBlock();
                                            newB->name = currentBlock->name + "$inlined_" + target->name + "$_" + tblock->name;
                                            blockMap[tblock->id] = newB->id;
                                        }
                                        BasicBlock* resumeBlock = fn.createBlock(currentBlock->name + "_resume");

                                        for (auto& tblock : target->blocks) {
                                            for (auto& tinst : tblock->instructions) {
                                                if (tinst.dest != -1 && vregMap.find(tinst.dest) == vregMap.end()) {
                                                    vregMap[tinst.dest] = fn.allocVReg();
                                                }
                                            }
                                        }

                                        for (auto& tblock : target->blocks) {
                                            for (auto& tinst : tblock->instructions) {
                                                if (tinst.op == OpCode::GET_PARAM) {
                                                    if (tinst.imm < inst.args.size()) vregMap[tinst.dest] = inst.args[tinst.imm];
                                                }
                                            }
                                        }

                                        int retReg = inst.dest != -1 ? inst.dest : fn.allocVReg();
                                        for (size_t k = i + 1; k < currentBlock->instructions.size(); ++k) {
                                            resumeBlock->instructions.push_back(currentBlock->instructions[k]);
                                        }

                                        for (auto& tblock : target->blocks) {
                                            BasicBlock* destBlock = nullptr;
                                            for (auto& b : fn.blocks) if (b->id == blockMap[tblock->id]) destBlock = b.get();

                                            for (auto& tinst : tblock->instructions) {
                                                if (tinst.op == OpCode::GET_PARAM) continue;
                                                if (tinst.op == OpCode::RET) {
                                                    if (tinst.src1 != -1 && inst.dest != -1) {
                                                        int srcReg = vregMap.count(tinst.src1) ? vregMap[tinst.src1] : tinst.src1;
                                                        destBlock->instructions.push_back({ OpCode::MOV, retReg, srcReg, -1, 0, inst.bytes });
                                                    }
                                                    destBlock->instructions.push_back({ OpCode::JMP, -1, -1, -1, (uint64_t)resumeBlock->id });
                                                }
                                                else {
                                                    Instruction cpy = tinst;

                                                    if (cpy.dest != -1) {
                                                        cpy.dest = vregMap[tinst.dest];
                                                    }
                                                    if (cpy.src1 != -1 && vregMap.count(cpy.src1)) cpy.src1 = vregMap[cpy.src1];
                                                    if (cpy.src2 != -1 && vregMap.count(cpy.src2)) cpy.src2 = vregMap[cpy.src2];
                                                    for (size_t argI = 0; argI < cpy.args.size(); ++argI) {
                                                        if (vregMap.count(cpy.args[argI])) cpy.args[argI] = vregMap[cpy.args[argI]];
                                                    }
                                                    if (cpy.op == OpCode::JMP || cpy.op == OpCode::JMP_FALSE) {
                                                        if (blockMap.count(cpy.imm)) cpy.imm = blockMap[cpy.imm];
                                                    }
                                                    destBlock->instructions.push_back(cpy);
                                                }
                                            }
                                        }
                                        newInsts.push_back({ OpCode::JMP, -1, -1, -1, (uint64_t)blockMap[target->blocks[0]->id] });
                                        didInlineHere = true;
                                        break;
                                    }
                                }
                            }
                            if (!didInlineHere) newInsts.push_back(inst);
                        }
                        if (didInlineHere) {
                            fn.blocks[bIdx]->instructions = std::move(newInsts);
                            inlinedAnything = true;
                            break; 
                        }
                    }
                    if (inlinedAnything) break;
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
                if (fn.isInline) continue;
                if (fn.isExported || fn.name.find("__standalone_") != std::string::npos) {
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




            std::cout << "[Optimizer] Running SCEV" << std::endl;
            bool scevChanged = true;
            while (scevChanged) {
                scevChanged = false;
                for (auto& fn : mod.functions) {
                    if (fn.isInline) continue;

                    std::map<int, int> totalDefs;
                    std::map<int, Instruction*> instMap;
                    for (auto& block : fn.blocks) {
                        for (auto& inst : block->instructions) {
                            if (inst.dest != -1) {
                                instMap[inst.dest] = &inst;
                                totalDefs[inst.dest]++;
                            }
                        }
                    }

                    std::map<int, uint64_t> globalConsts;
                    for (auto& block : fn.blocks) {
                        for (auto& inst : block->instructions) {
                            if (inst.dest != -1 && inst.op == OpCode::CONST && totalDefs[inst.dest] == 1) {
                                globalConsts[inst.dest] = inst.imm;
                            }
                        }
                    }

                    for (size_t bIdx = 0; bIdx < fn.blocks.size(); ++bIdx) {
                        auto& headerBlock = fn.blocks[bIdx];
                        if (headerBlock->instructions.empty()) continue;

                        bool isInlineFunc = false;
                        for (const auto& f : mod.functions) {
                            if (f.name == fn.name && f.isInline) {
                                isInlineFunc = true;
                                break;
                            }
                        }
                        if (isInlineFunc) continue;

                        int exitBlockId = -1;
                        int bodyBlockId = -1;
                        int condReg = -1;

                        for (auto& inst : headerBlock->instructions) {
                            if (inst.op == OpCode::JMP_FALSE) {
                                condReg = inst.src1;
                                exitBlockId = inst.imm;
                            }
                            else if (inst.op == OpCode::JMP) {
                                bodyBlockId = inst.imm;
                            }
                        }

                        if (exitBlockId == -1 || bodyBlockId == -1) continue;

                        BasicBlock* bodyBlock = nullptr;
                        bool isSingleBlockLoop = (bodyBlockId == headerBlock->id);

                        if (isSingleBlockLoop) {
                            bodyBlock = headerBlock.get();
                        }
                        else {
                            for (auto& b : fn.blocks) {
                                if (b->id == bodyBlockId) { bodyBlock = b.get(); break; }
                            }
                            if (!bodyBlock || bodyBlock->instructions.empty()
                                || bodyBlock->instructions.back().op != OpCode::JMP
                                || bodyBlock->instructions.back().imm != headerBlock->id) continue;
                        }

                        if (!bodyBlock || bodyBlock->instructions.empty()) continue;

                        Instruction* cmpInst = instMap.count(condReg) ? instMap[condReg] : nullptr;
                        if (!cmpInst || (cmpInst->op != OpCode::CMP_LT && cmpInst->op != OpCode::CMP_LE)) continue;

                        int indReg = cmpInst->src1;
                        uint64_t endVal = 0;
                        bool endValKnown = false;

                        std::map<int, uint64_t> initConst = globalConsts;
                        for (size_t prevIdx = 0; prevIdx < bIdx; ++prevIdx) {
                            for (auto& inst : fn.blocks[prevIdx]->instructions) {
                                if (inst.dest != -1) {
                                    if (inst.op == OpCode::CONST) initConst[inst.dest] = inst.imm;
                                    else if (inst.op == OpCode::MOV && inst.src1 != -1 && initConst.count(inst.src1)) {
                                        initConst[inst.dest] = initConst[inst.src1];
                                    }
                                }
                            }
                        }

                        if (cmpInst->src2 == -1) {
                            endVal = cmpInst->imm;
                            endValKnown = true;
                        }
                        else if (initConst.count(cmpInst->src2)) {
                            endVal = initConst[cmpInst->src2];
                            endValKnown = true;
                        }

                        if (!endValKnown) continue;
                        if (cmpInst->op == OpCode::CMP_LE) endVal += 1;

                        if (!initConst.count(indReg)) continue;
                        uint64_t startVal = initConst[indReg];
                        if (startVal >= endVal || (endVal - startVal) > 2000000ULL) continue;

                        auto loopStartIt = headerBlock->instructions.end();
                        for (auto it = headerBlock->instructions.begin(); it != headerBlock->instructions.end(); ++it) {
                            if (it->op == OpCode::JMP_FALSE || (cmpInst && it->dest == condReg)) {
                                loopStartIt = it;
                                break;
                            }
                        }

                        bool canSimulate = true;
                        std::unordered_set<int> mutatedRegs;
                        auto checkSafety = [&](const Instruction& inst) {
                            if (inst.isVolatile || inst.op == OpCode::STORE || inst.op == OpCode::LOAD ||
                                inst.op == OpCode::CALL || inst.op == OpCode::INLINE_ASM ||
                                inst.op == OpCode::RET || inst.op == OpCode::LOAD_LOCAL || inst.op == OpCode::STORE_LOCAL ||
                                inst.op == OpCode::GET_PARAM || inst.op == OpCode::VSTORE256 ||
                                inst.op == OpCode::VLOAD256 || inst.op == OpCode::VPBROADCASTQ) {
                                return false;
                            }
                            return true;
                        };

                        for (auto it = loopStartIt; it != headerBlock->instructions.end(); ++it) {
                            if (it->op == OpCode::JMP || it->op == OpCode::JMP_FALSE) continue;
                            if (!checkSafety(*it)) { canSimulate = false; break; }
                            if (it->dest != -1) mutatedRegs.insert(it->dest);
                        }

                        if (!isSingleBlockLoop) {
                            for (auto& inst : bodyBlock->instructions) {
                                if (inst.op == OpCode::JMP || inst.op == OpCode::JMP_FALSE) continue;
                                if (!checkSafety(inst)) { canSimulate = false; break; }
                                if (inst.dest != -1) mutatedRegs.insert(inst.dest);
                            }
                        }

                        if (!canSimulate) continue;

                        int maxReg = 0;
                        auto updateMaxReg = [&](const Instruction& inst) {
                            maxReg = std::max(maxReg, inst.dest);
                            maxReg = std::max(maxReg, inst.src1);
                            maxReg = std::max(maxReg, inst.src2);
                            for (int arg : inst.args) maxReg = std::max(maxReg, arg);
                        };

                        for (auto it = loopStartIt; it != headerBlock->instructions.end(); ++it) updateMaxReg(*it);
                        if (!isSingleBlockLoop) {
                            for (auto& inst : bodyBlock->instructions) updateMaxReg(inst);
                        }
                        for (auto& kv : initConst) maxReg = std::max(maxReg, kv.first);

                        std::vector<uint64_t> fastRegVals(maxReg + 1, 0);
                        std::vector<uint8_t> hasVal(maxReg + 1, 0);
                        for (auto& kv : initConst) {
                            fastRegVals[kv.first] = kv.second;
                            hasVal[kv.first] = 1;
                        }

                        std::vector<const Instruction*> headerInsts;
                        std::vector<const Instruction*> bodyInsts;

                        for (auto it = loopStartIt; it != headerBlock->instructions.end(); ++it) {
                            if (it->op != OpCode::JMP && it->op != OpCode::JMP_FALSE) headerInsts.push_back(&(*it));
                        }
                        if (!isSingleBlockLoop) {
                            for (auto& inst : bodyBlock->instructions) {
                                if (inst.op != OpCode::JMP && inst.op != OpCode::JMP_FALSE) bodyInsts.push_back(&inst);
                            }
                        }

                        auto simulateFast = [&](const Instruction* instPtr) {
                            const Instruction& inst = *instPtr;
                            if (inst.dest == -1) return;

                            uint64_t v1 = (inst.src1 != -1 && hasVal[inst.src1]) ? fastRegVals[inst.src1] : 0;
                            uint64_t v2 = (inst.src2 != -1) ? (hasVal[inst.src2] ? fastRegVals[inst.src2] : 0) : inst.imm;

                            switch (inst.op) {
                                case OpCode::CONST: fastRegVals[inst.dest] = inst.imm; break;
                                case OpCode::MOV:   fastRegVals[inst.dest] = v1; break;
                                case OpCode::ADD:   fastRegVals[inst.dest] = v1 + v2; break;
                                case OpCode::SUB:   fastRegVals[inst.dest] = v1 - v2; break;
                                case OpCode::MUL:   fastRegVals[inst.dest] = v1 * v2; break;
                                case OpCode::DIV:   if (v2 != 0) fastRegVals[inst.dest] = v1 / v2; break;
                                case OpCode::OR:    fastRegVals[inst.dest] = v1 | v2; break;
                                case OpCode::AND:   fastRegVals[inst.dest] = v1 & v2; break;
                                case OpCode::XOR:   fastRegVals[inst.dest] = v1 ^ v2; break;
                                case OpCode::SHL:   fastRegVals[inst.dest] = v1 << v2; break;
                                case OpCode::SHR:   fastRegVals[inst.dest] = v1 >> v2; break;
                                case OpCode::CMP_LT: fastRegVals[inst.dest] = v1 < v2; break;
                                case OpCode::CMP_LE: fastRegVals[inst.dest] = v1 <= v2; break;
                                case OpCode::CMP_EQ: fastRegVals[inst.dest] = v1 == v2; break;
                                case OpCode::CMP_NE: fastRegVals[inst.dest] = v1 != v2; break;
                                case OpCode::CMP_GT: fastRegVals[inst.dest] = v1 > v2; break;
                                case OpCode::CMP_GE: fastRegVals[inst.dest] = v1 >= v2; break;
                                case OpCode::CAST:  case OpCode::ZEXT: case OpCode::TRUNC:
                                    if (inst.bytes == 1) fastRegVals[inst.dest] = v1 & 0xFF;
                                    else if (inst.bytes == 2) fastRegVals[inst.dest] = v1 & 0xFFFF;
                                    else if (inst.bytes == 4) fastRegVals[inst.dest] = v1 & 0xFFFFFFFF;
                                    else fastRegVals[inst.dest] = v1;
                                    break;
                                default: break;
                            }
                            hasVal[inst.dest] = 1;
                        };

                        for (uint64_t iter = startVal; iter < endVal; ++iter) {
                            for (auto* inst : headerInsts) simulateFast(inst);
                            for (auto* inst : bodyInsts) simulateFast(inst);
                        }

                        for (auto* inst : headerInsts) simulateFast(inst);
                        std::vector<Instruction> flattenedInsts;
                        flattenedInsts.insert(flattenedInsts.end(), headerBlock->instructions.begin(), loopStartIt);

                        std::unordered_set<int> emitted;
                        for (auto it = loopStartIt; it != headerBlock->instructions.end(); ++it) {
                            if (it->dest != -1 && mutatedRegs.count(it->dest) && !emitted.count(it->dest)) {
                                emitted.insert(it->dest);
                                flattenedInsts.push_back({ OpCode::CONST, it->dest, -1, -1, fastRegVals[it->dest], it->bytes });
                            }
                        }

                        if (!isSingleBlockLoop) {
                            for (auto& inst : bodyBlock->instructions) {
                                if (inst.dest != -1 && mutatedRegs.count(inst.dest) && !emitted.count(inst.dest)) {
                                    emitted.insert(inst.dest);
                                    flattenedInsts.push_back({ OpCode::CONST, inst.dest, -1, -1, fastRegVals[inst.dest], inst.bytes });
                                }
                            }
                        }

                        flattenedInsts.push_back({ OpCode::JMP, -1, -1, -1, (uint64_t)exitBlockId });
                        headerBlock->instructions = std::move(flattenedInsts);

                        if (!isSingleBlockLoop) {
                            bodyBlock->instructions.clear();
                        }

                        scevChanged = true;
                        break;
                    }
                }
            }









            auto hasSideEffects = [](const Instruction& inst) {
                if (inst.isVolatile) return true;
                OpCode op = inst.op;
                return op == OpCode::STORE || op == OpCode::STORE_LOCAL ||
                    op == OpCode::CALL || op == OpCode::RET ||
                    op == OpCode::JMP || op == OpCode::JMP_FALSE ||
                    op == OpCode::INLINE_ASM ||
                    op == OpCode::GET_PARAM ||
                    op == OpCode::ALLOC;
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

                    std::map<int, OpCode> defOpcode;

                    for (auto& block : fn.blocks) {
                        for (auto& inst : block->instructions) {
                            if (inst.dest != -1) defOpcode[inst.dest] = inst.op;
                        }
                    }

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
                                if (inst.src1 != -1 && globalConsts.count(inst.src1)) {
                                    if (inst.op != OpCode::MOV && inst.op != OpCode::CAST && inst.op != OpCode::ZEXT && inst.op != OpCode::TRUNC) {
                                        if (inst.src2 != -1) {
                                            // inst.imm = globalConsts[inst.src1];
                                            // inst.src1 = -1; 
                                        }
                                    }
                                }
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

                            for (auto it = aliases.begin(); it != aliases.end(); ) {
                                if (it->second == inst.dest) it = aliases.erase(it);
                                else ++it;
                            }

                            for (auto it = ptrBase.begin(); it != ptrBase.end(); ) {
                                if (it->second == inst.dest) it = ptrBase.erase(it);
                                else ++it;
                            }

                            for (auto it = localToReg.begin(); it != localToReg.end(); ) {
                                if (it->second == inst.dest) it = localToReg.erase(it);
                                else ++it;
                            }

                            for (auto& kv : memStateReg) {
                                for (auto it = kv.second.begin(); it != kv.second.end(); ) {
                                    if (it->second == inst.dest) it = kv.second.erase(it);
                                    else ++it;
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
                                int depth = 0;
                                while ((aliases.count(curr) || globalAliases.count(curr)) && depth < 100) {
                                    curr = aliases.count(curr) ? aliases[curr] : globalAliases[curr];
                                    depth++;
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
                                    if (inst.op == OpCode::MOV || inst.op == OpCode::CAST ||
                                        inst.op == OpCode::ZEXT || inst.op == OpCode::TRUNC) {
                                        constants[inst.dest] = constants[inst.src1];
                                    }
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

                            auto ensureBase = [&](int reg) {
                                if (reg != -1 && !ptrBase.count(reg)) {
                                    if (defCounts[reg] == 0 || (defOpcode.count(reg) && (
                                        defOpcode[reg] == OpCode::LOAD ||
                                        defOpcode[reg] == OpCode::LOAD_LOCAL ||
                                        defOpcode[reg] == OpCode::GET_PARAM ||
                                        defOpcode[reg] == OpCode::ALLOC ||
                                        defOpcode[reg] == OpCode::CALL))) {
                                        ptrBase[reg] = reg;
                                        ptrOffset[reg] = 0;
                                    }
                                }
                            };

                            ensureBase(inst.src1);
                            ensureBase(inst.src2);

                            if (inst.op == OpCode::ALLOC || inst.op == OpCode::GET_PARAM || (inst.op == OpCode::CALL && inst.label == "calloc")) {
                                ptrBase[inst.dest] = inst.dest;
                                ptrOffset[inst.dest] = 0;
                            }

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
                                if (defCounts[inst.dest] == 1 && defCounts[inst.src1] == 1) {
                                    aliases[inst.dest] = inst.src1;
                                }

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

                            if (inst.op == OpCode::ADD) {
                                if (isConst(inst.src1, inst.imm) && isConst(inst.src2, inst.imm)) {
                                    inst.op = OpCode::CONST;
                                    inst.imm = getConst(inst.src1, inst.imm) + getConst(inst.src2, inst.imm);
                                    inst.src1 = -1; inst.src2 = -1;
                                    if (inst.dest != -1) constants[inst.dest] = inst.imm;
                                    changed = true;
                                }
                                else if (!isConst(inst.src1, inst.imm) && isConst(inst.src2, inst.imm) && inst.src2 != -1) {
                                    inst.imm = getConst(inst.src2, inst.imm);
                                    inst.src2 = -1;
                                    changed = true;
                                }
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
                            if (inst.op == OpCode::MUL || inst.op == OpCode::DIV) {
                                if (!isConst(inst.src1, 0) && isConst(inst.src2, inst.imm)) {
                                    uint64_t c = getConst(inst.src2, inst.imm);
                                    if (inst.op == OpCode::MUL && c == 1) {
                                        inst.op = OpCode::MOV;
                                        inst.src2 = -1;
                                        inst.imm = 0;
                                        changed = true;
                                    }
                                    else if (inst.op == OpCode::MUL && c == 0) {
                                        inst.op = OpCode::CONST;
                                        inst.src1 = -1;
                                        inst.src2 = -1;
                                        inst.imm = 0;
                                        changed = true;
                                    }
                                    else if (inst.op == OpCode::DIV && c == 1) {
                                        inst.op = OpCode::MOV;
                                        inst.src2 = -1;
                                        inst.imm = 0;
                                        changed = true;
                                    }
                                    else if (c > 0 && (c & (c - 1)) == 0) {
                                        uint64_t shift = 0;
                                        uint64_t temp = c;
                                        while ((temp & 1) == 0) { temp >>= 1; shift++; }
                                        inst.op = (inst.op == OpCode::MUL) ? OpCode::SHL : OpCode::SHR;
                                        inst.imm = shift;
                                        inst.src2 = -1;
                                        changed = true;
                                    }
                                }
                            }

                            if (inst.op == OpCode::ADD || inst.op == OpCode::SUB ||
                                inst.op == OpCode::MUL || inst.op == OpCode::DIV || inst.op == OpCode::UDIV ||
                                inst.op == OpCode::MOD || inst.op == OpCode::UMOD ||
                                inst.op == OpCode::SHL || inst.op == OpCode::SHR ||
                                inst.op == OpCode::OR || inst.op == OpCode::XOR ||
                                inst.op == OpCode::AND) {

                                if (isConst(inst.src1, inst.imm) && isConst(inst.src2, inst.imm)) {
                                    uint64_t v1 = (inst.src1 == -1) ? inst.imm : getConst(inst.src1, 0);
                                    uint64_t v2 = (inst.src2 == -1) ? inst.imm : getConst(inst.src2, 0);
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
                                        case OpCode::UDIV: if (v2 != 0) res = v1 / v2; else canFold = false; break;
                                        case OpCode::MOD:  if (v2 != 0) res = (int64_t)v1 % (int64_t)v2; else canFold = false; break;
                                        case OpCode::UMOD: if (v2 != 0) res = v1 % v2; else canFold = false; break;
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

                            if ((inst.op == OpCode::CAST || inst.op == OpCode::MOV || inst.op == OpCode::ZEXT || inst.op == OpCode::TRUNC) && isConst(inst.src1, 0)) {
                                uint64_t val = getConst(inst.src1, 0);
                                if (inst.bytes == 1) val &= 0xFF;
                                else if (inst.bytes == 2) val &= 0xFFFF;
                                else if (inst.bytes == 4) val &= 0xFFFFFFFF;
                                inst.op = OpCode::CONST;
                                inst.imm = val;
                                inst.src1 = -1;
                                changed = true;
                                if (inst.dest != -1) constants[inst.dest] = val;
                            }

                            if (inst.op == OpCode::ADD && inst.dest != -1) {
                                if (inst.src1 != -1 && ptrBase.count(inst.src1)) {
                                    if (isConst(inst.src2, inst.imm)) {
                                        ptrBase[inst.dest] = ptrBase[inst.src1];
                                        ptrOffset[inst.dest] = ptrOffset[inst.src1] + getConst(inst.src2, inst.imm);
                                    }
                                }
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

                                if (isConst(inst.src1, inst.imm) && isConst(inst.src2, inst.imm)) {
                                    int64_t v1 = (inst.src1 == -1) ? inst.imm : getConst(inst.src1, 0);
                                    int64_t v2 = (inst.src2 == -1) ? inst.imm : getConst(inst.src2, 0);

                                    if (inst.src1 == -1 && inst.src2 == -1) continue;

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

                            if (inst.op == OpCode::JMP_FALSE && inst.src1 != -1 && isConst(inst.src1, 0)) {
                                uint64_t cond = getConst(inst.src1, 0);
                                if (cond == 0) {
                                    inst.op = OpCode::JMP;
                                    inst.src1 = -1;
                                    changed = true;
                                }
                                else {
                                    changed = true;
                                    newInsts.push_back(inst);
                                    deadInsts.push_back(true);
                                    continue;
                                }
                            }

                            if (inst.op == OpCode::SELECT && isConst(inst.src1, inst.imm)) {
                                uint64_t condVal = getConst(inst.src1, inst.imm);

                                inst.op = OpCode::MOV;
                                inst.src1 = (condVal != 0) ? inst.src2 : inst.args[0];
                                inst.src2 = -1;
                                inst.imm = 0;
                                inst.args.clear();

                                changed = true;
                            }

                            if (inst.op == OpCode::ADD || inst.op == OpCode::SUB ||
                                inst.op == OpCode::MUL || inst.op == OpCode::DIV ||
                                inst.op == OpCode::CAST || inst.op == OpCode::LEA_LOCAL ||
                                inst.op == OpCode::CMP_EQ || inst.op == OpCode::CMP_NE ||
                                inst.op == OpCode::CMP_LT || inst.op == OpCode::CMP_GT ||
                                inst.op == OpCode::CMP_LE || inst.op == OpCode::CMP_GE ||
                                inst.op == OpCode::OR || inst.op == OpCode::XOR ||
                                inst.op == OpCode::SHL || inst.op == OpCode::SHR) {

                                bool hasMutatedOperand = (inst.src1 != -1 && defCounts[inst.src1] > 1) ||
                                    (inst.src2 != -1 && defCounts[inst.src2] > 1);

                                if (hasMutatedOperand ||
                                    (inst.src1 == -1 && inst.src2 == -1) ||
                                    (inst.op == OpCode::ADD && inst.src1 == -1) ||
                                    (inst.op == OpCode::SUB && inst.src1 == -1)) {
                                }
                                else {
                                    ExprVal ev{ inst.op, inst.src1, inst.src2, inst.imm };

                                    if (availableExprs.count(ev) && availableExprs[ev] != inst.dest) {
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
                            }

                            if (inst.op == OpCode::BSWAP && isConst(inst.src1, 0)) {
                                uint64_t val = getConst(inst.src1, 0);
                                uint64_t res = 0;

                                if (inst.bytes == 2) {
                                    res = ((val >> 8) & 0xFF) | ((val & 0xFF) << 8);
                                }
                                else if (inst.bytes == 4) {
                                    res = ((val >> 24) & 0xFF) |
                                        ((val >> 8) & 0xFF00) |
                                        ((val & 0xFF00) << 8) |
                                        ((val & 0xFF) << 24);
                                }
                                else if (inst.bytes == 8) {
                                    // :3                im so full
                                    res = ((val & 0x00000000000000FFULL) << 56) |
                                        ((val & 0x000000000000FF00ULL) << 40) |
                                        ((val & 0x0000000000FF0000ULL) << 24) |
                                        ((val & 0x00000000FF000000ULL) << 8) |
                                        ((val & 0x000000FF00000000ULL) >> 8) |
                                        ((val & 0x0000FF0000000000ULL) >> 24) |
                                        ((val & 0x00FF000000000000ULL) >> 40) |
                                        ((val & 0xFF00000000000000ULL) >> 56);
                                }

                                inst.op = OpCode::CONST;
                                inst.src1 = -1;
                                inst.src2 = -1;
                                inst.imm = res;
                                changed = true;
                                if (inst.dest != -1) constants[inst.dest] = res;
                            }










                            if (inst.op == OpCode::ADD) {
                                if (isConst(inst.src1, inst.imm) && isConst(inst.src2, inst.imm)) {
                                    uint64_t c1 = getConst(inst.src1, inst.imm);
                                    uint64_t c2 = getConst(inst.src2, inst.imm);
                                    inst.op = OpCode::CONST;
                                    inst.src1 = -1; inst.src2 = -1;
                                    inst.imm = c1 + c2;
                                    changed = true;
                                }
                                else if (!isConst(inst.src1, inst.imm) && isConst(inst.src2, inst.imm) && inst.src2 != -1) {
                                    uint64_t c2 = getConst(inst.src2, inst.imm);
                                    inst.src2 = -1;
                                    inst.imm = c2;
                                    changed = true;
                                }
                                else if (isConst(inst.src1, inst.imm) && !isConst(inst.src2, inst.imm) && inst.src1 != -1 && inst.src2 != -1) {
                                    uint64_t c1 = getConst(inst.src1, inst.imm);
                                    inst.src1 = inst.src2;
                                    inst.src2 = -1;
                                    inst.imm = c1;
                                    changed = true;
                                }
                            }
                            else if (inst.op == OpCode::SUB) {
                                if (isConst(inst.src1, inst.imm) && isConst(inst.src2, inst.imm)) {
                                    uint64_t c1 = getConst(inst.src1, inst.imm);
                                    uint64_t c2 = getConst(inst.src2, inst.imm);
                                    inst.op = OpCode::CONST;
                                    inst.src1 = -1; inst.src2 = -1;
                                    inst.imm = c1 - c2;
                                    changed = true;
                                }
                                else if (!isConst(inst.src1, inst.imm) && isConst(inst.src2, inst.imm) && inst.src2 != -1) {
                                    uint64_t c2 = getConst(inst.src2, inst.imm);
                                    inst.src2 = -1;
                                    inst.imm = c2;
                                    changed = true;
                                }
                                else if (inst.src1 != -1 && inst.src2 != -1) {
                                    int realSrc1 = resolve(inst.src1);
                                    int realSrc2 = resolve(inst.src2);

                                    if (realSrc1 == realSrc2) {
                                        inst.op = OpCode::CONST;
                                        inst.src1 = -1; inst.src2 = -1;
                                        inst.imm = 0;
                                        changed = true;
                                    }
                                }
                            }
                            else if (inst.op == OpCode::CAST || inst.op == OpCode::MOV) {
                                if (inst.src1 != -1 && ptrBase.count(inst.src1)) {
                                    ptrBase[inst.dest] = ptrBase[inst.src1];
                                    ptrOffset[inst.dest] = ptrOffset[inst.src1];
                                }
                            }
                            else if (inst.op == OpCode::ALLOC || inst.op == OpCode::GET_PARAM) {
                                ptrBase[inst.dest] = inst.dest;
                                ptrOffset[inst.dest] = 0;
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
                                std::set<int> passedBases;
                                for (int arg : inst.args) {
                                    if (ptrBase.count(arg)) passedBases.insert(ptrBase[arg]);
                                    else passedBases.insert(arg);
                                }
                                for (auto& prevInst : newInsts) {
                                    if (prevInst.op == OpCode::STORE && prevInst.src2 != -1) {
                                        if (ptrBase.count(prevInst.src2)) passedBases.insert(ptrBase[prevInst.src2]);
                                        else passedBases.insert(prevInst.src2);
                                    }
                                    if (prevInst.op == OpCode::STORE_LOCAL && prevInst.src1 != -1) {
                                        if (ptrBase.count(prevInst.src1)) passedBases.insert(ptrBase[prevInst.src1]);
                                        else passedBases.insert(prevInst.src1);
                                    }
                                }
                                std::vector<int> toErase;
                                for (auto& kv : memStateSize) {
                                    int base = kv.first;
                                    bool isSafe = false;
                                    if (defOpcode.count(base) == 0) {
                                        isSafe = true;
                                    }
                                    else {
                                        OpCode defOp = defOpcode[base];
                                        isSafe = (defOp == OpCode::ALLOC || defOp == OpCode::CALL || defOp == OpCode::GET_PARAM);
                                    }
                                    if (!isSafe || passedBases.count(base)) {
                                        toErase.push_back(base);
                                    }
                                }
                                for (int base : toErase) {
                                    memStateReg.erase(base);
                                    memStateImm.erase(base);
                                    memStateSize.erase(base);
                                    lastStoreIdx.erase(base);
                                }
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
                                else{
                                    int storeBase = (inst.src1 != -1 && ptrBase.count(inst.src1)) ? ptrBase[inst.src1] : -1;

                                    if (storeBase != -1) {
                                        memStateReg.erase(storeBase);
                                        memStateImm.erase(storeBase);
                                        memStateSize.erase(storeBase);
                                        lastStoreIdx.erase(storeBase);
                                    }
                                    else {
                                        std::set<int> passedBases;
                                        for (auto& prevInst : newInsts) {
                                            if (prevInst.op == OpCode::STORE && prevInst.src2 != -1) {
                                                if (ptrBase.count(prevInst.src2) && !ptrBase.count(prevInst.src1))
                                                    passedBases.insert(ptrBase[prevInst.src2]);
                                            }
                                        }
                                        std::vector<int> toErase;
                                        for (auto& kv : memStateSize) {
                                            int base = kv.first;
                                            bool isSafe = defOpcode.count(base) && (defOpcode[base] == OpCode::ALLOC || defOpcode[base] == OpCode::CALL || defOpcode[base] == OpCode::GET_PARAM);
                                            if (!isSafe || passedBases.count(base)) {
                                                toErase.push_back(base);
                                            }
                                        }
                                        for (int base : toErase) {
                                            memStateReg.erase(base);
                                            memStateImm.erase(base);
                                            memStateSize.erase(base);
                                            lastStoreIdx.erase(base);
                                        }
                                    }
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
                                    std::cout << "[DEBUG Peephole] SROA memory forward matching imm base->CONST in " << inst.toString() << "\n";
                                    inst.op = OpCode::CONST;
                                    inst.imm = memStateImm[base][offset];
                                    inst.src1 = -1;
                                    inst.src2 = -1;
                                    constants[inst.dest] = inst.imm;
                                    changed = true;
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
                                std::cout << "[DEBUG Peephole] DSE: Eliminating unused register def v" << inst.dest << " in " << inst.toString() << "\n";
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

                                newInsts.push_back(inst);
                                deadInsts.push_back(true);
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
                                std::cout << "[DEBUG Peephole] DSE: Eliminating dead STORE_LOCAL to local[" << inst.imm << "]\n";
                                changed = true;
                                newInsts.push_back(inst);
                                deadInsts.push_back(true);
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
                if (fnPass2Changed) { std::cout << "[DEBUG Global] Peephole requested global pass restart\n"; globalPassChanged = true; }
            }









            

            std::cout << "[Optimizer] Running vectorizer" << std::endl;
            bool vecChanged = true;
            while (vecChanged) {
                vecChanged = false;
                for (auto& fn : mod.functions) {
                    int seqVecReg = -1;
                    size_t numBlocks = fn.blocks.size();
                    for (size_t bIdx = 0; bIdx < numBlocks; ++bIdx) {
                        auto* block = fn.blocks[bIdx].get();
                        if (block->instructions.empty()) continue;

                        if (block->instructions.back().op == OpCode::JMP && block->instructions.back().imm == block->id) {
                            if (vectorizeLoop(fn, block, seqVecReg)) {
                                std::cout << "[DEBUG Global] Vectorizer requested global pass restart\n";
                                vecChanged = true;
                                globalPassChanged = true;
                            }
                        }
                    }
                }
            }











            std::cout << "[Optimizer] Running SROA" << std::endl;

            for (auto& fn : mod.functions) {
                if (fn.blocks.empty()) continue;
                std::unordered_set<BasicBlock*> visited;
                std::vector<BasicBlock*> orderedPtrs;

                auto dfs = [&](auto& self, BasicBlock* curr) -> void {
                    if (!curr || !visited.insert(curr).second) return;
                    orderedPtrs.push_back(curr);

                    bool fallsThrough = true;
                    BasicBlock* tTarget = nullptr;
                    BasicBlock* fTarget = nullptr;

                    for (auto& inst : curr->instructions) {
                        if (inst.op == OpCode::JMP || inst.op == OpCode::JMP_FALSE) {
                            for (auto& b : fn.blocks) {
                                if (b->id == static_cast<int>(inst.imm)) {
                                    if (inst.op == OpCode::JMP_FALSE) fTarget = b.get();
                                    else tTarget = b.get();
                                    break;
                                }
                            }
                            if (inst.op == OpCode::JMP) fallsThrough = false;
                        }
                        else if (inst.op == OpCode::RET) fallsThrough = false;
                    }

                    if (tTarget) self(self, tTarget);
                    if (fTarget) self(self, fTarget);

                    if (fallsThrough) {
                        auto it = std::find_if(fn.blocks.begin(), fn.blocks.end(), [&](const auto& mb) { return mb.get() == curr; });
                        if (it != fn.blocks.end() && std::next(it) != fn.blocks.end()) {
                            self(self, std::next(it)->get());
                        }
                    }
                };

                dfs(dfs, fn.blocks[0].get());

                if (orderedPtrs.size() == fn.blocks.size()) {
                    std::vector<std::unique_ptr<BasicBlock>> reordered;
                    for (auto* ptr : orderedPtrs) {
                        for (auto& b : fn.blocks) {
                            if (b.get() == ptr) {
                                reordered.push_back(std::move(b));
                                break;
                            }
                        }
                    }
                    fn.blocks = std::move(reordered);
                }
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
                                escapes[inst.dest] = false;
                                rootBase[inst.dest] = inst.dest;
                                rootOffset[inst.dest] = 0;
                            }
                            if (inst.op == OpCode::CALL && inst.label == "calloc" && inst.dest != -1) {
                                escapes[inst.dest] = false;
                                rootBase[inst.dest] = inst.dest;
                                rootOffset[inst.dest] = 0;
                            }
                        }
                    }

                    std::map<int, uint64_t> sroaConsts;
                    std::map<int, int> assignCount;
                    for (auto& block : fn.blocks) {
                        for (auto& inst : block->instructions) {
                            if (inst.dest != -1) {
                                if (inst.op == OpCode::CONST) {
                                    sroaConsts[inst.dest] = inst.imm;
                                    assignCount[inst.dest]++;
                                }
                                else if (inst.op == OpCode::MOV || inst.op == OpCode::CAST || inst.op == OpCode::ZEXT || inst.op == OpCode::TRUNC) {
                                    assignCount[inst.dest]++;
                                }
                            }
                        }
                    }

                    bool constsChanged = true;
                    while (constsChanged) {
                        constsChanged = false;
                        for (auto& block : fn.blocks) {
                            for (auto& inst : block->instructions) {
                                if (inst.dest != -1 && assignCount[inst.dest] == 1) {
                                    if (inst.op == OpCode::MOV || inst.op == OpCode::CAST || inst.op == OpCode::ZEXT || inst.op == OpCode::TRUNC) {
                                        if (inst.src1 != -1 && sroaConsts.count(inst.src1)) {
                                            if (!sroaConsts.count(inst.dest)) {
                                                sroaConsts[inst.dest] = sroaConsts[inst.src1];
                                                constsChanged = true;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    bool trackingChanged = true;
                    while (trackingChanged) {
                        trackingChanged = false;
                        for (auto& block : fn.blocks) {
                            for (auto& inst : block->instructions) {
                                if ((inst.op == OpCode::MOV || inst.op == OpCode::CAST || inst.op == OpCode::ZEXT || inst.op == OpCode::SEXT || inst.op == OpCode::TRUNC || inst.op == OpCode::LOAD) && inst.dest != -1) {
                                    if (rootBase.count(inst.src1) && !rootBase.count(inst.dest)) {
                                        rootBase[inst.dest] = rootBase[inst.src1];
                                        rootOffset[inst.dest] = rootOffset[inst.src1];
                                        trackingChanged = true;
                                    }
                                }

                                if (inst.op == OpCode::ADD && inst.dest != -1) {
                                    int offset = 0;
                                    bool valid = false;
                                    if (inst.src2 == -1) {
                                        offset = static_cast<int>(inst.imm);
                                        valid = true;
                                    }
                                    else if (sroaConsts.count(inst.src2)) {
                                        offset = static_cast<int>(sroaConsts[inst.src2]);
                                        valid = true;
                                    }
                                    if (valid && rootBase.count(inst.src1) && !rootBase.count(inst.dest)) {
                                        rootBase[inst.dest] = rootBase[inst.src1];
                                        rootOffset[inst.dest] = rootOffset[inst.src1] + offset;
                                        trackingChanged = true;
                                    }
                                    else if (!valid) {
                                        if (rootBase.count(inst.src1)) escapes[rootBase[inst.src1]] = true;
                                        if (inst.src2 != -1 && rootBase.count(inst.src2)) escapes[rootBase[inst.src2]] = true;
                                    }
                                }
                            }
                        }
                    }

                    auto markEscape = [&](int reg) {
                        if (reg != -1 && rootBase.count(reg)) escapes[rootBase[reg]] = true;
                    };

                    /*
                    std::map<int, std::vector<std::pair<int, int>>> sroaAccesses;
                    std::map<int, std::set<int>> sroaBlocks;
                    std::map<int, std::set<int>> sroaDependencies;

                    for (auto& block : fn.blocks) {
                        for (auto& inst : block->instructions) {
                            if (inst.op == OpCode::CALL || inst.op == OpCode::STORE_LOCAL || inst.op == OpCode::RET || inst.op == OpCode::GET_PARAM) {
                                markEscape(inst.src1);
                                markEscape(inst.src2);
                                if (inst.op == OpCode::CALL) {
                                    for (int arg : inst.args) {
                                        if (inst.label == "free") continue;
                                        markEscape(arg);
                                    }
                                }
                            }
                            if (inst.op == OpCode::STORE) {
                                int ptrReg = -1;
                                int valReg = -1;

                                if (inst.isVolatile) {
                                    markEscape(inst.src1); markEscape(inst.src2); markEscape(inst.dest);
                                    //newInsts.push_back(inst);
                                    continue;
                                }

                                if (inst.src1 != -1 && rootBase.count(inst.src1)) {
                                    ptrReg = inst.src1;
                                    valReg = inst.src2 != -1 ? inst.src2 : -1;
                                }
                                //if (rootBase.count(inst.dest)) { ptrReg = inst.dest; valReg = inst.src1; }
                                //else if (inst.src1 != -1 && rootBase.count(inst.src1)) { ptrReg = inst.src1; valReg = inst.src2 != -1 ? inst.src2 : inst.dest; }
                                //else if (inst.src2 != -1 && rootBase.count(inst.src2)) { ptrReg = inst.src2; valReg = inst.src1; }

                                if (ptrReg != -1) {
                                    int base = rootBase[ptrReg];
                                    sroaAccesses[base].push_back({ rootOffset[ptrReg], inst.bytes });
                                    sroaBlocks[base].insert(block->id);

                                    if (valReg != -1 && rootBase.count(valReg)) {
                                        sroaDependencies[base].insert(rootBase[valReg]);
                                        //escapes[rootBase[valReg]] = true;
                                    }
                                }
                                else {
                                    bool didEscape = false;
                                    if (inst.src1 != -1 && rootBase.count(inst.src1) && !escapes[rootBase[inst.src1]]) { escapes[rootBase[inst.src1]] = true; didEscape = true; }
                                    if (inst.src2 != -1 && rootBase.count(inst.src2) && !escapes[rootBase[inst.src2]]) { escapes[rootBase[inst.src2]] = true; didEscape = true; }
                                    if (inst.dest != -1 && rootBase.count(inst.dest) && !escapes[rootBase[inst.dest]]) { escapes[rootBase[inst.dest]] = true; didEscape = true; }
                                }
                            }
                            if (inst.op == OpCode::LOAD) {
                                int ptrReg = inst.src1;

                                if (inst.isVolatile) {
                                    markEscape(inst.src1);
                                    //newInsts.push_back(inst);
                                    continue;
                                }

                                if (ptrReg != -1 && rootBase.count(ptrReg)) {
                                    int base = rootBase[ptrReg];
                                    sroaAccesses[base].push_back({ rootOffset[ptrReg], inst.bytes });
                                    sroaBlocks[base].insert(block->id);
                                }
                                else {
                                    if (ptrReg != -1 && rootBase.count(ptrReg) && !escapes[rootBase[ptrReg]]) {
                                        escapes[rootBase[ptrReg]] = true;
                                        //sroaChanged = true;
                                    }
                                    //newInsts.push_back(inst);
                                }
                            }
                        }
                    }*/

                    std::map<int, std::vector<std::pair<int, int>>> sroaAccesses;
                    std::map<int, std::set<int>> sroaBlocks;
                    std::map<int, std::set<int>> sroaDependencies;

                    for (auto& block : fn.blocks) {
                        for (auto& inst : block->instructions) {
                            if (inst.op == OpCode::CALL || inst.op == OpCode::STORE_LOCAL || inst.op == OpCode::RET || inst.op == OpCode::GET_PARAM) {
                                markEscape(inst.src1);
                                markEscape(inst.src2);
                                if (inst.op == OpCode::CALL) {
                                    for (int arg : inst.args) {
                                        if (inst.label == "free") continue;
                                        markEscape(arg);
                                    }
                                }
                            }

                            if (inst.op == OpCode::STORE) {
                                if (inst.isVolatile) {
                                    markEscape(inst.src1); markEscape(inst.src2); markEscape(inst.dest);
                                    continue;
                                }

                                int ptrReg = -1;
                                int valReg = -1;
                                if (inst.src1 != -1 && rootBase.count(inst.src1)) {
                                    ptrReg = inst.src1;
                                    valReg = inst.src2 != -1 ? inst.src2 : -1;
                                }

                                if (ptrReg != -1) {
                                    int base = rootBase[ptrReg];
                                    sroaAccesses[base].push_back({ rootOffset[ptrReg], inst.bytes });
                                    sroaBlocks[base].insert(block->id);
                                    if (valReg != -1 && rootBase.count(valReg)) {
                                        sroaDependencies[base].insert(rootBase[valReg]);
                                    }
                                }
                                else {
                                    if (inst.src1 != -1 && rootBase.count(inst.src1) && !escapes[rootBase[inst.src1]]) { escapes[rootBase[inst.src1]] = true; }
                                    if (inst.src2 != -1 && rootBase.count(inst.src2) && !escapes[rootBase[inst.src2]]) { escapes[rootBase[inst.src2]] = true; }
                                    if (inst.dest != -1 && rootBase.count(inst.dest) && !escapes[rootBase[inst.dest]]) { escapes[rootBase[inst.dest]] = true; }
                                }
                            }

                            if (inst.op == OpCode::LOAD) {
                                if (inst.isVolatile) {
                                    markEscape(inst.src1);
                                    continue;
                                }

                                int ptrReg = inst.src1;
                                if (ptrReg != -1 && rootBase.count(ptrReg)) {
                                    int base = rootBase[ptrReg];
                                    sroaAccesses[base].push_back({ rootOffset[ptrReg], inst.bytes });
                                    sroaBlocks[base].insert(block->id);
                                }
                                else {
                                    if (ptrReg != -1 && rootBase.count(ptrReg) && !escapes[rootBase[ptrReg]]) {
                                        escapes[rootBase[ptrReg]] = true;
                                    }
                                }
                            }
                        }
                    }

                    for (auto& kv : sroaAccesses) {
                        int base = kv.first;
                        bool overlap = false;

                        auto& accs = kv.second;
                        if (accs.size() > 1) {
                            std::sort(accs.begin(), accs.end());
                            accs.erase(std::unique(accs.begin(), accs.end()), accs.end());

                            for (size_t i = 0; i < accs.size(); i++) {
                                for (size_t j = i + 1; j < accs.size(); j++) {
                                    int o1 = accs[i].first, s1 = accs[i].second;
                                    int o2 = accs[j].first, s2 = accs[j].second;
                                    if (o1 < o2 + s2 && o1 + s1 > o2) {
                                        if (o1 != o2 || s1 != s2) {
                                            overlap = true;
                                            break;
                                        }
                                    }
                                }
                                if (overlap) break;
                            }
                        }

                        if (overlap) escapes[base] = true;
                    }

                    bool sroaPropChanged = true;
                    while (sroaPropChanged) {
                        sroaPropChanged = false;
                        for (auto& kv : sroaDependencies) {
                            if (escapes[kv.first]) {
                                for (int val : kv.second) {
                                    if (!escapes[val]) {
                                        escapes[val] = true;
                                        sroaPropChanged = true;
                                    }
                                }
                            }
                        }
                    }

                    //for (auto& kv : sroaBlocks) {
                    //    if (kv.second.size() > 1) {
                    //        escapes[kv.first] = true;
                    //    }
                    //}

                    /*
                    std::map<int, std::map<int, int>> sroaRegs;
                    auto getSroaReg = [&](int base, int offset) {
                        if (!sroaRegs[base].count(offset)) sroaRegs[base][offset] = fn.allocVReg();
                        return sroaRegs[base][offset];
                    };

                    auto updateSroaReg = [&](int base, int offset) {
                        int newReg = fn.allocVReg();
                        sroaRegs[base][offset] = newReg;
                        return newReg;
                    };

                    for (auto& block : fn.blocks) {
                        std::vector<Instruction> newInsts;
                        for (auto& inst : block->instructions) {
                            if (inst.op == OpCode::ALLOC && escapes.count(inst.dest) && !escapes[inst.dest]) {
                                sroaChanged = true;
                                anySroaChanged = true;
                                continue;
                            }
                            if (inst.op == OpCode::CALL && inst.label == "calloc" && escapes.count(inst.dest) && !escapes[inst.dest]) {
                                sroaChanged = true;
                                anySroaChanged = true;
                                continue;
                            }

                            if (inst.op == OpCode::CALL && inst.label == "free") {
                                int ptrReg = inst.args.empty() ? -1 : inst.args[0];
                                if (ptrReg != -1 && rootBase.count(ptrReg)) {
                                    if (!escapes[rootBase[ptrReg]]) {
                                        sroaChanged = true;
                                        anySroaChanged = true;
                                        continue;
                                    }
                                }
                            }

                            if (inst.op == OpCode::STORE) {
                                int ptrReg = -1;
                                int valReg = -1;
                                if (rootBase.count(inst.dest)) {
                                    ptrReg = inst.dest;
                                    valReg = inst.src1;
                                }
                                else if (inst.src1 != -1 && rootBase.count(inst.src1)) {
                                    ptrReg = inst.src1;
                                    valReg = inst.src2 != -1 ? inst.src2 : -1;
                                }

                                if (ptrReg != -1) {
                                    int base = rootBase[ptrReg];
                                    if (!escapes[base]) {
                                        int offset = rootOffset[ptrReg];
                                        int staticReg = updateSroaReg(base, offset);
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
                                else {
                                    bool didEscape = false;
                                    if (inst.src1 != -1 && !rootBase.count(inst.src1)) {
                                        if (inst.src2 != -1 && rootBase.count(inst.src2)) { escapes[rootBase[inst.src2]] = true; didEscape = true; }
                                    }
                                    if (inst.dest != -1 && !rootBase.count(inst.dest)) {
                                        if (inst.src1 != -1 && rootBase.count(inst.src1)) { escapes[rootBase[inst.src1]] = true; didEscape = true; }
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
                    }*/
                    std::map<int, std::map<int, int>> sroaRegs;
                    auto getSroaReg = [&](int base, int offset) {
                        if (!sroaRegs[base].count(offset)) sroaRegs[base][offset] = fn.allocVReg();
                        return sroaRegs[base][offset];
                    };
                    auto updateSroaReg = [&](int base, int offset) {
                        int newReg = fn.allocVReg();
                        sroaRegs[base][offset] = newReg;
                        return newReg;
                    };

                    for (auto& block : fn.blocks) {
                        std::vector<Instruction> newInsts;
                        for (auto& inst : block->instructions) {
                            if (inst.op == OpCode::ALLOC && escapes.count(inst.dest) && !escapes[inst.dest]) {
                                sroaChanged = true;
                                anySroaChanged = true;
                                continue;
                            }
                            if (inst.op == OpCode::CALL && inst.label == "calloc" && escapes.count(inst.dest) && !escapes[inst.dest]) {
                                sroaChanged = true;
                                anySroaChanged = true;
                                continue;
                            }
                            if (inst.op == OpCode::CALL && inst.label == "free") {
                                int ptrReg = inst.args.empty() ? -1 : inst.args[0];
                                if (ptrReg != -1 && rootBase.count(ptrReg)) {
                                    if (!escapes[rootBase[ptrReg]]) {
                                        sroaChanged = true;
                                        anySroaChanged = true;
                                        continue;
                                    }
                                }
                            }

                            if (inst.op == OpCode::STORE) {
                                int ptrReg = -1;
                                int valReg = -1;
                                if (rootBase.count(inst.dest)) {
                                    ptrReg = inst.dest;
                                    valReg = inst.src1;
                                }
                                else if (inst.src1 != -1 && rootBase.count(inst.src1)) {
                                    ptrReg = inst.src1;
                                    valReg = inst.src2 != -1 ? inst.src2 : -1;
                                }

                                if (ptrReg != -1) {
                                    int base = rootBase[ptrReg];
                                    if (!escapes[base]) {
                                        int offset = rootOffset[ptrReg];
                                        int staticReg = updateSroaReg(base, offset);
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
                                else {
                                    bool didEscape = false;
                                    if (inst.src1 != -1 && !rootBase.count(inst.src1)) {
                                        if (inst.src2 != -1 && rootBase.count(inst.src2)) { escapes[rootBase[inst.src2]] = true; didEscape = true; }
                                    }
                                    if (inst.dest != -1 && !rootBase.count(inst.dest)) {
                                        if (inst.src1 != -1 && rootBase.count(inst.src1)) { escapes[rootBase[inst.src1]] = true; didEscape = true; }
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
            if (anySroaChanged) { std::cout << "[DEBUG Global] SROA requested global pass restart\n"; globalPassChanged = true; }












            std::cout << "[Optimizer] Running cfgr" << std::endl;
            bool cfgChanged = true;
            bool anyCfgChanged = false;

            while (cfgChanged) {
                cfgChanged = false;

                for (auto& fn : mod.functions) {
                    if (fn.blocks.empty()) continue;

                    bool fnCfgChanged = false;

                    std::unordered_set<BasicBlock*> reachable;
                    std::vector<BasicBlock*> worklist;
                    reachable.insert(fn.blocks[0].get());
                    worklist.push_back(fn.blocks[0].get());

                    while (!worklist.empty()) {
                        BasicBlock* curr = worklist.back();
                        worklist.pop_back();

                        bool fallsThrough = true;

                        for (auto& inst : curr->instructions) {
                            if (inst.op == OpCode::JMP || inst.op == OpCode::JMP_FALSE) {
                                BasicBlock* targetBlock = nullptr;
                                for (auto& b : fn.blocks) {
                                    if (b->id == static_cast<int>(inst.imm)) {
                                        targetBlock = b.get();
                                        break;
                                    }
                                }
                                if (targetBlock && reachable.insert(targetBlock).second) {
                                    worklist.push_back(targetBlock);
                                }
                                if (inst.op == OpCode::JMP) fallsThrough = false;
                            }
                            else if (inst.op == OpCode::RET) {
                                fallsThrough = false;
                            }
                        }

                        if (fallsThrough) {
                            auto it = std::find_if(fn.blocks.begin(), fn.blocks.end(), [&](const std::unique_ptr<BasicBlock>& mb) {
                                return mb.get() == curr;
                                });
                            if (it != fn.blocks.end() && std::next(it) != fn.blocks.end()) {
                                BasicBlock* nextBlock = std::next(it)->get();
                                if (reachable.insert(nextBlock).second) {
                                    worklist.push_back(nextBlock);
                                }
                            }
                        }
                    }

                    if (reachable.size() < fn.blocks.size()) {
                        std::vector<std::unique_ptr<BasicBlock>> reachableBlocks;
                        for (auto& b : fn.blocks) {
                            if (reachable.count(b.get())) {
                                reachableBlocks.push_back(std::move(b));
                            }
                        }
                        fn.blocks = std::move(reachableBlocks);
                        fnCfgChanged = true;
                        cfgChanged = true;
                        anyCfgChanged = true;
                    }

                    for (auto& b : fn.blocks) {
                        for (size_t k = 0; k < b->instructions.size(); ++k) {
                            if (b->instructions[k].op == OpCode::JMP || b->instructions[k].op == OpCode::RET) {
                                if (b->instructions.size() > k + 1) {
                                    b->instructions.resize(k + 1);
                                    fnCfgChanged = true;
                                    cfgChanged = true;
                                    anyCfgChanged = true;
                                }
                                break;
                            }
                        }
                    }

                    std::unordered_map<int, int> jmpTargets;
                    for (auto& b : fn.blocks) {
                        if (b->instructions.size() == 1 && b->instructions[0].op == OpCode::JMP) {
                            jmpTargets[b->id] = static_cast<int>(b->instructions[0].imm);
                        }
                    }

                    for (auto& b : fn.blocks) {
                        for (auto& inst : b->instructions) {
                            if (inst.op == OpCode::JMP || inst.op == OpCode::JMP_FALSE) {
                                int curr = static_cast<int>(inst.imm);
                                std::unordered_set<int> visited;

                                while (jmpTargets.count(curr) && visited.insert(curr).second) {
                                    curr = jmpTargets[curr];
                                }

                                if (curr != inst.imm && curr != b->id) {
                                    inst.imm = curr;
                                    fnCfgChanged = true;
                                    cfgChanged = true;
                                    anyCfgChanged = true;
                                }
                            }
                        }
                    }

                    if (fnCfgChanged) {
                        continue;
                    }

                    bool blockMerged = false;
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
                                    if (!targetBlock->instructions.empty()) {
                                        b->instructions.pop_back();
                                        b->instructions.insert(b->instructions.end(),
                                            targetBlock->instructions.begin(),
                                            targetBlock->instructions.end());
                                        targetBlock->instructions.clear();

                                        blockMerged = true;
                                        cfgChanged = true;
                                        anyCfgChanged = true;
                                    }
                                }
                            }
                        }
                    }

                    if (blockMerged) {
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
            if (anyCfgChanged) { std::cout << "[DEBUG Global] CFGR requested global pass restart\n"; globalPassChanged = true; }

             







            std::cout << "[Optimizer] Running iv" << std::endl;
            bool ivChanged = true;
            while (ivChanged) {
                ivChanged = false;
                for (auto& fn : mod.functions) {
                    std::map<int, uint64_t> initConst;
                    std::map<int, int> constBlock;
                    std::map<int, std::vector<std::pair<int, Instruction>>> mods;
                    std::map<int, int> totalDefs;
                    std::map<int, Instruction*> instMap;
                    std::map<int, uint64_t> globalConsts;

                    for (auto& block : fn.blocks) {
                        for (auto& inst : block->instructions) {
                            if (inst.dest != -1) {
                                instMap[inst.dest] = &inst;
                                totalDefs[inst.dest]++;
                                if (inst.op == OpCode::CONST) {
                                    globalConsts[inst.dest] = inst.imm;
                                }
                            }
                        }
                    }

                    auto getRealSrc = [&](int reg) {
                        int curr = reg;
                        int depth = 0;
                        while (instMap.count(curr) && instMap[curr]->op == OpCode::MOV && instMap[curr]->src1 != -1 && depth < 100) {
                            curr = instMap[curr]->src1;
                            depth++;
                        }
                        return curr;
                    };

                    for (auto& block : fn.blocks) {
                        for (auto& inst : block->instructions) {
                            if (inst.dest != -1) {
                                if (inst.op == OpCode::CONST) {
                                    if (initConst.find(inst.dest) == initConst.end()) {
                                        initConst[inst.dest] = inst.imm;
                                        constBlock[inst.dest] = block->id;
                                    }
                                }
                                else if (inst.op == OpCode::MOV && inst.src1 != -1 && globalConsts.count(inst.src1)) {
                                    if (initConst.find(inst.dest) == initConst.end()) {
                                        initConst[inst.dest] = globalConsts[inst.src1];
                                        constBlock[inst.dest] = block->id;
                                    }
                                }

                                if (inst.op == OpCode::MOV && inst.src1 != -1) {
                                    int realSrc = getRealSrc(inst.src1);
                                    if (instMap.count(realSrc)) {
                                        Instruction* srcInst = instMap[realSrc];
                                        if (srcInst->op == OpCode::ADD || srcInst->op == OpCode::SUB || srcInst->op == OpCode::MUL) {
                                            mods[inst.dest].push_back({ block->id, *srcInst });
                                        }
                                    }
                                }
                                else if (inst.op == OpCode::ADD || inst.op == OpCode::SUB || inst.op == OpCode::MUL) {
                                    mods[inst.dest].push_back({ block->id, inst });
                                }
                            }
                        }
                    }

                    std::map<int, int> replaceMap;
                    std::vector<int> regs;
                    for (auto& kv : initConst) regs.push_back(kv.first);

                    for (size_t i = 0; i < regs.size(); ++i) {
                        for (size_t j = i + 1; j < regs.size(); ++j) {
                            int r1 = regs[i];
                            int r2 = regs[j];
                            if (replaceMap.count(r1) || replaceMap.count(r2)) continue;

                            if (initConst[r1] == initConst[r2] && constBlock[r1] == constBlock[r2] && totalDefs[r1] == totalDefs[r2]) {
                                if (mods[r1].size() == mods[r2].size() && !mods[r1].empty()) {
                                    bool same = true;
                                    for (size_t k = 0; k < mods[r1].size(); ++k) {
                                        if (mods[r1][k].first != mods[r2][k].first) { same = false; break; }

                                        auto& m1 = mods[r1][k].second;
                                        auto& m2 = mods[r2][k].second;

                                        if (m1.op != m2.op || m1.imm != m2.imm || m1.bytes != m2.bytes) {
                                            same = false; break;
                                        }

                                        auto checkOp = [&](int op1, int op2) {
                                            if (op1 == op2) return true;
                                            if (op1 == -1 || op2 == -1) return false;

                                            int real1 = getRealSrc(op1);
                                            int real2 = getRealSrc(op2);
                                            if (real1 == real2) return true;

                                            int realR1 = getRealSrc(r1);
                                            int realR2 = getRealSrc(r2);
                                            if ((real1 == realR1 && real2 == realR2) ||
                                                (real1 == realR2 && real2 == realR1)) return true;
                                            return false;
                                            };

                                        if (!checkOp(m1.src1, m2.src1) || !checkOp(m1.src2, m2.src2)) {
                                            same = false; break;
                                        }
                                    }
                                    if (same) {
                                        replaceMap[r2] = r1;
                                    }
                                }
                            }
                        }
                    }

                    if (!replaceMap.empty()) {
                        bool actuallyChanged = false;
                        for (auto& block : fn.blocks) {
                            for (auto& inst : block->instructions) {
                                if (inst.src1 != -1 && replaceMap.count(inst.src1)) {
                                    if (inst.src1 != replaceMap[inst.src1]) {
                                        inst.src1 = replaceMap[inst.src1];
                                        actuallyChanged = true;
                                    }
                                }
                                if (inst.src2 != -1 && replaceMap.count(inst.src2)) {
                                    if (inst.src2 != replaceMap[inst.src2]) {
                                        inst.src2 = replaceMap[inst.src2];
                                        actuallyChanged = true;
                                    }
                                }
                                for (auto& arg : inst.args) {
                                    if (replaceMap.count(arg)) {
                                        if (arg != replaceMap[arg]) {
                                            arg = replaceMap[arg];
                                            actuallyChanged = true;
                                        }
                                    }
                                }
                                if (inst.dest != -1 && replaceMap.count(inst.dest)) {
                                    if (inst.op != OpCode::CONST && inst.op != OpCode::MOV) {
                                        inst.op = OpCode::MOV;
                                        inst.src1 = replaceMap[inst.dest];
                                        inst.src2 = -1;
                                        inst.imm = 0;
                                        inst.bytes = 8;
                                        actuallyChanged = true;
                                    }
                                }
                            }
                        }

                        if (actuallyChanged) {
                            std::cout << "[DEBUG Global] IV requested global pass restart\n";
                            ivChanged = true;
                            globalPassChanged = true;
                        }
                    }
                }
            }






















            std::cout << "[Optimizer] Running licm" << std::endl;
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

                                    while (insertIt != entryBlock->instructions.begin()) {
                                        auto prevIt = std::prev(insertIt);
                                        if (prevIt->op == OpCode::JMP || prevIt->op == OpCode::JMP_FALSE || prevIt->op == OpCode::RET) {
                                            insertIt = prevIt;
                                        }
                                        else {
                                            break;
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