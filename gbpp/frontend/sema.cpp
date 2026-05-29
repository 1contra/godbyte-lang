#include "../include/sema.hpp"
#include <algorithm>
#include <iostream>
#include <set>

namespace gbpp {

    static int s_loopDepth = 0;

    Sema::Sema() {}

    void Sema::error(SourceLoc loc, const std::string& msg) {
        errors.push_back("Line " + std::to_string(loc.line) + ":" + std::to_string(loc.col) + " - " + msg);
    }

    bool Sema::analyze(Program& prog) {
        errors.clear();
        s_loopDepth = 0;
        enterScope();

        for (auto& st : prog.structs) {
            if (!st->genericParams.empty()) {
                m_generic_structs[st->name] = st.get();
            }
            else {
                m_structs[st->name] = st.get();
            }
        }

        for (auto& fn : prog.functions) {
            if (!fn->genericParams.empty()) {
                m_generic_functions[fn->name] = fn.get();
            }
            else {
                m_functions[fn->name] = fn.get();
                m_pending_function_checks.push_back(fn.get());
            }
        }

        for (auto& c : prog.constants) {
            m_constants[c->name] = c.get();

            Type* expectedType = resolveType(c->parsedType);
            checkExpr(*c->initializer);

            if (expectedType && c->initializer->type && *expectedType != *c->initializer->type) {
                error(c->loc, "Type mismatch in constant '" + c->name + "'");
            }
        }

        for (auto& enm : prog.enums) m_enums[enm->name] = enm.get();
        for (auto& alias : prog.aliases) {
            m_aliases[alias->name] = alias->targetType;
        }

        for (auto& st : prog.structs) {
            if (st->genericParams.empty()) {
                int runningOffset = 0;
                for (auto& field : st->fields) {
                    if (field.offset > 0 && field.offset < runningOffset) {
                        error(st->loc, "Struct field overlap detected in '" + st->name + "'.");
                    }

                    if (field.offset == 0 && runningOffset != 0) field.offset = runningOffset;

                    Type* fieldType = resolveType(field.parsedType);
                    int fieldSize = fieldType ? fieldType->sizeBytes : 8;
                    runningOffset = std::max(runningOffset, field.offset) + fieldSize;
                }
            }
        }

        size_t checkIdx = 0;
        while (checkIdx < m_pending_function_checks.size()) {
            FunctionDecl* fn = m_pending_function_checks[checkIdx];
            checkFunction(*fn);
            checkIdx++;
        }

        exitScope();

        for (auto& st : m_instantiated_structs) {
            prog.structs.push_back(std::move(st));
        }
        m_instantiated_structs.clear();

        for (auto& fn : m_instantiated_functions) {
            prog.functions.push_back(std::move(fn));
        }
        m_instantiated_functions.clear();

        for (const auto& err : errors) {
            std::cerr << "[Sema Error] " << err << "\n";
        }
        return errors.empty();
    }

    bool Sema::analyzeModules(const std::vector<Program*>& progs) {
        errors.clear();
        s_loopDepth = 0;
        enterScope();

        for (auto prog : progs) {
            for (auto& st : prog->structs) {
                if (!st->genericParams.empty()) {
                    m_generic_structs[st->name] = st.get();
                }
                else {
                    m_structs[st->name] = st.get();
                }
            }
            for (auto& fn : prog->functions) {
                if (!fn->genericParams.empty()) {
                    m_generic_functions[fn->name] = fn.get();
                }
                else {
                    m_functions[fn->name] = fn.get();
                    m_pending_function_checks.push_back(fn.get());
                }
            }
            for (auto& enm : prog->enums) m_enums[enm->name] = enm.get();
            for (auto& alias : prog->aliases) m_aliases[alias->name] = alias->targetType;
        }

        for (auto prog : progs) {
            for (auto& st : prog->structs) {
                if (st->genericParams.empty()) {
                    int runningOffset = 0;
                    for (auto& field : st->fields) {
                        if (field.offset > 0 && field.offset < runningOffset) {
                            error(st->loc, "Struct field overlap detected in '" + st->name + "'.");
                        }
                        if (field.offset == 0 && runningOffset != 0) field.offset = runningOffset;
                        Type* fieldType = resolveType(field.parsedType);
                        int fieldSize = fieldType ? fieldType->sizeBytes : 8;
                        runningOffset = std::max(runningOffset, field.offset) + fieldSize;
                    }
                }
            }
        }

        size_t checkIdx = 0;
        while (checkIdx < m_pending_function_checks.size()) {
            FunctionDecl* fn = m_pending_function_checks[checkIdx];
            checkFunction(*fn);
            checkIdx++;
        }

        exitScope();

        if (!progs.empty()) {
            for (auto& st : m_instantiated_structs) {
                progs.back()->structs.push_back(std::move(st));
            }
            m_instantiated_structs.clear();

            for (auto& fn : m_instantiated_functions) {
                progs.back()->functions.push_back(std::move(fn));
            }
            m_instantiated_functions.clear();
        }

        for (const auto& err : errors) {
            std::cerr << "[Sema Error] " << err << "\n";
        }
        return errors.empty();
    }

    Type* Sema::resolveType(const ParsedType& pt) {
        ParsedType actual = pt;

        while (m_aliases.count(actual.baseName)) {
            ParsedType target = m_aliases[actual.baseName];
            actual.baseName = target.baseName;
            actual.genericArgs = target.genericArgs;
            actual.isFunction = target.isFunction;
            actual.paramTypes = target.paramTypes;
            actual.returnType = target.returnType;
            if (!target.modifiers.empty()) {
                actual.modifiers.insert(actual.modifiers.end(), target.modifiers.begin(), target.modifiers.end());
            }

            if (target.isArray && !actual.isArray) {
                actual.isArray = true;
                actual.arraySizeExpr = target.arraySizeExpr;
            }
        }

        if (actual.isFunction) {
            Type* t = new Type{ ScalarType::FunctionPtr, actual.toString(), 8 };
            for (const auto& p : actual.paramTypes) {
                t->paramTypes.push_back(resolveType(p));
            }
            t->returnType = actual.returnType ? resolveType(*actual.returnType) : &TypeVoid;
            return t;
        }

        std::string bName = actual.baseName;

        if (!actual.genericArgs.empty()) {
            std::string mangledName = bName;
            for (const auto& arg : actual.genericArgs) {
                Type* argType = resolveType(arg);
                if (argType) {
                    mangledName += "_" + argType->name;
                }
                else {
                    mangledName += "_" + arg.baseName;
                }
            }

            if (m_generic_structs.count(bName)) {
                if (!m_structs.count(mangledName)) {
                    instantiateStruct(m_generic_structs[bName], actual.genericArgs, mangledName);
                }
                bName = mangledName;
            }
            else if (m_structs.count(mangledName)) {
                bName = mangledName;
            }
            else {
                bName = mangledName;
            }
            actual.genericArgs.clear();
        }

        Type* baseType = nullptr;

        if (bName == "u64") baseType = &TypeU64;
        else if (bName == "u32") baseType = &TypeU32;
        else if (bName == "u16") baseType = &TypeU16;
        else if (bName == "u8")  baseType = &TypeU8;
        else if (bName == "i64") baseType = &TypeI64;
        else if (bName == "i32") baseType = &TypeI32;
        else if (bName == "i16") baseType = &TypeI16;
        else if (bName == "i8")  baseType = &TypeI8;
        else if (bName == "f32") baseType = &TypeF32;
        else if (bName == "f64") baseType = &TypeF64;
        else if (bName == "bool") baseType = &TypeBool;
        else if (bName == "void") baseType = &TypeVoid;
        else if (m_enums.count(bName)) {
            baseType = new Type{ TypeU64.scalar, bName, 8 };
        }
        else if (m_structs.count(bName)) {
            baseType = new Type{ ScalarType::Struct, bName, 0 };
        }
        else {
            return nullptr;
        }

        if (actual.isArray) {
            if (actual.arraySizeExpr.empty()) {
                baseType = new Type{
                    ScalarType::Pointer,
                    actual.toString(),
                    8,
                    false, false, true,
                    baseType
                };
            }
            else {
                int arrSize = 0;
                bool isDynamic = false;
                try {
                    arrSize = std::stoi(actual.arraySizeExpr);
                }
                catch (...) {
                    isDynamic = true;
                }
                baseType = new Type{
                    ScalarType::Struct,
                    actual.toString(),
                    isDynamic ? 0 : (baseType->sizeBytes * arrSize),
                    false, false, true,
                    baseType
                };
            }
        }

        for (auto it = actual.modifiers.rbegin(); it != actual.modifiers.rend(); ++it) {
            baseType = new Type{
                ScalarType::Pointer,
                actual.toString(),
                8,
                false, false, false,
                baseType
            };
        }

        return baseType;
    }

    ParsedType Sema::substituteType(ParsedType pt, const std::unordered_map<std::string, std::string>& subs) {
        if (subs.count(pt.baseName)) {
            pt.baseName = subs.at(pt.baseName);
        }
        if (pt.isArray && !pt.arraySizeExpr.empty() && subs.count(pt.arraySizeExpr)) {
            pt.arraySizeExpr = subs.at(pt.arraySizeExpr);
        }
        for (auto& arg : pt.genericArgs) {
            arg = substituteType(arg, subs);
        }
        return pt;
    }

    std::unique_ptr<Expr> Sema::cloneExpr(Expr* e, const std::unordered_map<std::string, std::string>& subs) {
        if (!e) return nullptr;
        if (auto lit = dynamic_cast<IntLiteral*>(e)) {
            auto res = std::make_unique<IntLiteral>(); res->loc = lit->loc; res->value = lit->value; return res;
        }
        if (auto lit = dynamic_cast<FloatLiteral*>(e)) {
            auto res = std::make_unique<FloatLiteral>(); res->loc = lit->loc; res->value = lit->value; return res;
        }
        if (auto lit = dynamic_cast<StringLiteral*>(e)) {
            auto res = std::make_unique<StringLiteral>(); res->loc = lit->loc; res->value = lit->value; return res;
        }
        if (auto lit = dynamic_cast<NullLiteral*>(e)) {
            auto res = std::make_unique<NullLiteral>(); res->loc = lit->loc; return res;
        }
        if (auto v = dynamic_cast<VarExpr*>(e)) {
            auto res = std::make_unique<VarExpr>(); res->loc = v->loc; res->name = v->name; return res;
        }
        if (auto d = dynamic_cast<DerefExpr*>(e)) {
            auto res = std::make_unique<DerefExpr>(); res->loc = d->loc; res->operand = cloneExpr(d->operand.get(), subs); return res;
        }
        if (auto a = dynamic_cast<ArrayAccessExpr*>(e)) {
            auto res = std::make_unique<ArrayAccessExpr>(); res->loc = a->loc; res->array = cloneExpr(a->array.get(), subs); res->index = cloneExpr(a->index.get(), subs); return res;
        }
        if (auto c = dynamic_cast<CallExpr*>(e)) {
            auto res = std::make_unique<CallExpr>(); res->loc = c->loc; res->callee = cloneExpr(c->callee.get(), subs);
            for (auto& arg : c->args) res->args.push_back(cloneExpr(arg.get(), subs)); return res;
        }
        if (auto si = dynamic_cast<StructInitExpr*>(e)) {
            auto res = std::make_unique<StructInitExpr>(); res->loc = si->loc;
            ParsedType pt; pt.baseName = si->structName;
            res->structName = substituteType(pt, subs).baseName;
            for (auto& f : si->fields) res->fields.push_back({ f.name, cloneExpr(f.value.get(), subs) }); return res;
        }
        if (auto sz = dynamic_cast<SizeofExpr*>(e)) {
            auto res = std::make_unique<SizeofExpr>(); res->loc = sz->loc; res->parsedTargetType = substituteType(sz->parsedTargetType, subs); return res;
        }
        if (auto c = dynamic_cast<CastExpr*>(e)) {
            auto res = std::make_unique<CastExpr>(); res->loc = c->loc; res->castKind = c->castKind; res->parsedTargetType = substituteType(c->parsedTargetType, subs); res->operand = cloneExpr(c->operand.get(), subs); return res;
        }
        if (auto m = dynamic_cast<MemberExpr*>(e)) {
            auto res = std::make_unique<MemberExpr>(); res->loc = m->loc; res->object = cloneExpr(m->object.get(), subs); res->memberName = m->memberName; return res;
        }
        if (auto a = dynamic_cast<AllocExpr*>(e)) {
            auto res = std::make_unique<AllocExpr>(); res->loc = a->loc; res->parsedTargetType = substituteType(a->parsedTargetType, subs);
            for (auto& arg : a->args) res->args.push_back(cloneExpr(arg.get(), subs)); return res;
        }
        if (auto a = dynamic_cast<AssignmentExpr*>(e)) {
            auto res = std::make_unique<AssignmentExpr>(); res->loc = a->loc; res->op = a->op; res->target = cloneExpr(a->target.get(), subs); res->value = cloneExpr(a->value.get(), subs); return res;
        }
        if (auto b = dynamic_cast<BinaryExpr*>(e)) {
            auto res = std::make_unique<BinaryExpr>(); res->loc = b->loc; res->op = b->op; res->left = cloneExpr(b->left.get(), subs); res->right = cloneExpr(b->right.get(), subs); return res;
        }
        if (auto un = dynamic_cast<UnaryExpr*>(e)) {
            auto res = std::make_unique<UnaryExpr>(); res->loc = un->loc; res->op = un->op; res->operand = cloneExpr(un->operand.get(), subs); return res;
        }
        if (auto addr = dynamic_cast<AddrOfExpr*>(e)) {
            auto res = std::make_unique<AddrOfExpr>(); res->loc = addr->loc; res->operand = cloneExpr(addr->operand.get(), subs); return res;
        }
        if (auto ea = dynamic_cast<EnumAccessExpr*>(e)) {
            auto res = std::make_unique<EnumAccessExpr>(); res->loc = ea->loc; res->enumName = ea->enumName; res->memberName = ea->memberName; return res;
        }
        if (auto sf = dynamic_cast<SelfFieldExpr*>(e)) {
            auto res = std::make_unique<SelfFieldExpr>(); res->loc = sf->loc; res->fieldName = sf->fieldName; return res;
        }
        return nullptr;
    }

    std::unique_ptr<Stmt> Sema::cloneStmt(Stmt* s, const std::unordered_map<std::string, std::string>& subs) {
        if (!s) return nullptr;
        if (auto v = dynamic_cast<VarDecl*>(s)) {
            auto res = std::make_unique<VarDecl>(); res->loc = v->loc; res->name = v->name; res->attributes = v->attributes;
            res->parsedType = substituteType(v->parsedType, subs);
            res->initializer = cloneExpr(v->initializer.get(), subs); return res;
        }
        if (auto asmS = dynamic_cast<AsmStmt*>(s)) {
            auto res = std::make_unique<AsmStmt>(); res->loc = asmS->loc; res->assembly = asmS->assembly; return res;
        }
        if (auto ret = dynamic_cast<ReturnStmt*>(s)) {
            auto res = std::make_unique<ReturnStmt>(); res->loc = ret->loc; res->value = cloneExpr(ret->value.get(), subs); return res;
        }
        if (auto e = dynamic_cast<ExprStmt*>(s)) {
            auto res = std::make_unique<ExprStmt>(); res->loc = e->loc; res->expr = cloneExpr(e->expr.get(), subs); return res;
        }
        if (auto b = dynamic_cast<BlockStmt*>(s)) {
            auto res = std::make_unique<BlockStmt>(); res->loc = b->loc; for (auto& stmt : b->statements) res->statements.push_back(cloneStmt(stmt.get(), subs)); return res;
        }
        if (auto i = dynamic_cast<IfStmt*>(s)) {
            auto res = std::make_unique<IfStmt>(); res->loc = i->loc; res->condition = cloneExpr(i->condition.get(), subs); res->thenBranch = cloneStmt(i->thenBranch.get(), subs); res->elseBranch = cloneStmt(i->elseBranch.get(), subs); return res;
        }
        if (auto w = dynamic_cast<WhileStmt*>(s)) {
            auto res = std::make_unique<WhileStmt>(); res->loc = w->loc; res->condition = cloneExpr(w->condition.get(), subs); res->body = cloneStmt(w->body.get(), subs); return res;
        }
        if (auto f = dynamic_cast<ForStmt*>(s)) {
            auto res = std::make_unique<ForStmt>(); res->loc = f->loc; res->init = cloneStmt(f->init.get(), subs); res->condition = cloneExpr(f->condition.get(), subs); res->update = cloneExpr(f->update.get(), subs); res->body = cloneStmt(f->body.get(), subs); return res;
        }
        if (auto b = dynamic_cast<BreakStmt*>(s)) {
            auto res = std::make_unique<BreakStmt>(); res->loc = b->loc; return res;
        }
        return nullptr;
    }

    FunctionDecl* Sema::instantiateFunction(FunctionDecl* tmpl, const std::vector<ParsedType>& args, const std::string& mangledName) {
        std::unordered_map<std::string, std::string> substitutions;
        for (size_t i = 0; i < args.size(); ++i) {
            Type* argType = resolveType(args[i]);
            substitutions[tmpl->genericParams[i].name] = argType ? argType->name : args[i].baseName;
        }

        auto inst = std::make_unique<FunctionDecl>();
        inst->loc = tmpl->loc;
        inst->name = mangledName;
        inst->attributes = tmpl->attributes;

        for (const auto& p : tmpl->params) {
            inst->params.push_back({ p.name, substituteType(p.parsedType, substitutions), nullptr });
        }
        inst->returnType = substituteType(tmpl->returnType, substitutions);

        if (tmpl->body) {
            inst->body = std::unique_ptr<BlockStmt>(static_cast<BlockStmt*>(cloneStmt(tmpl->body.get(), substitutions).release()));
        }

        FunctionDecl* rawPtr = inst.get();
        m_functions[mangledName] = rawPtr;
        m_instantiated_functions.push_back(std::move(inst));

        m_pending_function_checks.push_back(rawPtr);

        return rawPtr;
    }

    StructDecl* Sema::instantiateStruct(StructDecl* tmpl, const std::vector<ParsedType>& args, const std::string& mangledName) {
        if (args.size() != tmpl->genericParams.size()) {
            error(tmpl->loc, "Generic argument count mismatch for " + tmpl->name);
            return nullptr;
        }

        auto inst = std::make_unique<StructDecl>();
        inst->name = mangledName;
        inst->loc = tmpl->loc;

        StructDecl* rawPtr = inst.get();
        m_structs[mangledName] = rawPtr;

        std::unordered_map<std::string, std::string> substitutions;
        for (size_t i = 0; i < args.size(); ++i) {
            Type* argType = resolveType(args[i]);
            substitutions[tmpl->genericParams[i].name] = argType ? argType->name : args[i].baseName;
        }

        int runningOffset = 0;
        for (const auto& f : tmpl->fields) {
            ParsedType subbedType = substituteType(f.parsedType, substitutions);

            Type* resolvedFType = resolveType(subbedType);
            int fieldSize = resolvedFType ? resolvedFType->sizeBytes : 8;

            int offset = f.offset;
            if (offset == 0 && runningOffset != 0) offset = runningOffset;

            inst->fields.push_back({ f.name, subbedType, offset });
            runningOffset = std::max(runningOffset, offset) + fieldSize;
        }

        m_instantiated_structs.push_back(std::move(inst));

        std::string prefix = tmpl->name + "_";
        for (auto& [gName, gFn] : m_generic_functions) {
            if (gName.starts_with(prefix)) {
                std::string methodName = gName.substr(prefix.length());
                std::string mangledFnName = mangledName + "_" + methodName;

                if (!m_functions.count(mangledFnName)) {
                    instantiateFunction(gFn, args, mangledFnName);
                }
            }
        }

        return rawPtr;
    }

    void Sema::checkFunction(FunctionDecl& fn) {
        fn.returnTypeResolved = resolveType(fn.returnType);
        if (!fn.returnTypeResolved) fn.returnTypeResolved = &TypeVoid;

        m_currentFunctionReturnType = fn.returnTypeResolved;

        enterScope();
        for (auto& param : fn.params) {
            param.resolvedType = resolveType(param.parsedType);
            if (!param.resolvedType) {
                error(fn.loc, "Unknown type " + param.parsedType.toString());
                param.resolvedType = &TypeVoid;
            }
            declareVariable(param.name, param.resolvedType, fn.loc);
        }
        if (fn.body) {
            checkBlock(*fn.body);
        }
        else if (fn.attributes.count("extern") == 0) {
            error(fn.loc, "Function '" + fn.name + "' has no body. To declare an external interface, use [[@extern]]");
        }
        exitScope();

        m_currentFunctionReturnType = nullptr;
    }

    void Sema::checkBlock(BlockStmt& block) {
        enterScope();
        for (auto& stmt : block.statements) checkStmt(*stmt);

        for (const auto& owner : m_currentScope->ownedVars) {
            if (m_currentScope->freedVars.find(owner) == m_currentScope->freedVars.end()) {
                error(block.loc, "Memory leak detected. Variable '" + owner + "' is an 'owner' type but was never freed. Fix: Call 'free(" + owner + ");' or annotate with '[[@nofree]]'.");
            }
        }
        exitScope();
    }

    void Sema::checkStmt(Stmt& stmt) {
        if (auto d = dynamic_cast<VarDecl*>(&stmt)) {
            d->resolvedType = resolveType(d->parsedType);
            if (!d->resolvedType) {
                error(d->loc, "Unknown type " + d->parsedType.toString());
                d->resolvedType = &TypeVoid;
            }

            if (d->initializer) {
                checkExpr(*d->initializer);

                bool isAlloc = dynamic_cast<AllocExpr*>(d->initializer.get()) != nullptr;

                if (d->resolvedType && d->resolvedType->isPointer() && isAlloc) {

                }
                else if (d->resolvedType && d->initializer->type && *d->resolvedType != *d->initializer->type) {
                    error(d->loc, "Type mismatch in declaration of '" + d->name +
                        "'. Expected " + d->resolvedType->toString() + ", got " + d->initializer->type->toString());
                }
            }
            declareVariable(d->name, d->resolvedType, d->loc, d->attributes);
        }
        else if (auto asmStmt = dynamic_cast<AsmStmt*>(&stmt)) {

        }
        else if (auto b = dynamic_cast<BreakStmt*>(&stmt)) {
            if (s_loopDepth == 0) {
                error(b->loc, "'break' statement not within a loop.");
            }
        }
        else if (auto r = dynamic_cast<ReturnStmt*>(&stmt)) {
            if (r->value) {
                checkExpr(*r->value);

                if (m_currentFunctionReturnType == &TypeVoid) {
                    error(r->loc, "Function returning 'void' cannot return a value.");
                }
                else {
                    bool isAllocPtr = m_currentFunctionReturnType->isPointer() && dynamic_cast<AllocExpr*>(r->value.get()) != nullptr;
                    if (!isAllocPtr && *r->value->type != *m_currentFunctionReturnType) {
                        error(r->loc, "Return type mismatch. Expected " +
                            m_currentFunctionReturnType->toString() + ", got " + r->value->type->toString());
                    }
                }
            }
            else {
                if (m_currentFunctionReturnType != &TypeVoid) {
                    error(r->loc, "Function must return a value of type " + m_currentFunctionReturnType->toString());
                }
            }
        }
        else if (auto e = dynamic_cast<ExprStmt*>(&stmt)) {
            checkExpr(*e->expr);
        }
        else if (auto i = dynamic_cast<IfStmt*>(&stmt)) {
            checkExpr(*i->condition);
            checkStmt(*i->thenBranch);
            if (i->elseBranch) checkStmt(*i->elseBranch);
        }
        else if (auto w = dynamic_cast<WhileStmt*>(&stmt)) {
            checkExpr(*w->condition);
            s_loopDepth++;
            checkStmt(*w->body);
            s_loopDepth--;
        }
        else if (auto f = dynamic_cast<ForStmt*>(&stmt)) {
            enterScope();

            if (f->init) checkStmt(*f->init);

            if (f->condition) {
                checkExpr(*f->condition);
            }

            if (f->update) {
                checkExpr(*f->update);
            }

            s_loopDepth++;
            if (f->body) checkStmt(*f->body);
            s_loopDepth--;

            exitScope();
        }
        else if (auto b = dynamic_cast<BlockStmt*>(&stmt)) {
            checkBlock(*b);
        }
    }

    void Sema::checkExpr(Expr& expr) {
        if (auto lit = dynamic_cast<IntLiteral*>(&expr)) {
            if (lit->value.ends_with("u8"))       lit->type = &TypeU8;
            else if (lit->value.ends_with("u16")) lit->type = &TypeU16;
            else if (lit->value.ends_with("u32")) lit->type = &TypeU32;
            else if (lit->value.ends_with("u64")) lit->type = &TypeU64;
            else if (lit->value.ends_with("i8"))  lit->type = &TypeI8;
            else if (lit->value.ends_with("i16")) lit->type = &TypeI16;
            else if (lit->value.ends_with("i32")) lit->type = &TypeI32;
            else if (lit->value.ends_with("i64")) lit->type = &TypeI64;
            else                                  lit->type = &TypeU64;
        }
        else if (auto nullLit = dynamic_cast<NullLiteral*>(&expr)) {
            nullLit->type = &TypeNull;
        }
        else if (auto enumAcc = dynamic_cast<EnumAccessExpr*>(&expr)) {
            if (!m_enums.count(enumAcc->enumName)) {
                error(enumAcc->loc, "Unknown enum: " + enumAcc->enumName);
                enumAcc->type = &TypeVoid;
                return;
            }

            EnumDecl* enm = m_enums[enumAcc->enumName];
            bool found = false;
            for (auto& member : enm->members) {
                if (member.name == enumAcc->memberName) {
                    enumAcc->value = member.value;
                    found = true;
                    break;
                }
            }

            if (!found) {
                error(enumAcc->loc, "Enum member '" + enumAcc->memberName + "' not found in enum '" + enumAcc->enumName + "'");
                enumAcc->type = &TypeVoid;
                return;
            }

            ParsedType pt; pt.baseName = enumAcc->enumName;
            enumAcc->type = resolveType(pt);
        }
        else if (auto structInit = dynamic_cast<StructInitExpr*>(&expr)) {
            ParsedType pt; pt.baseName = structInit->structName;
            structInit->type = resolveType(pt);

            if (!structInit->type || structInit->type->scalar != ScalarType::Struct) {
                error(structInit->loc, "Unknown struct type: " + structInit->structName);
                structInit->type = &TypeVoid;
                return;
            }

            StructDecl* st = m_structs[structInit->type->name];
            for (auto& initField : structInit->fields) {
                checkExpr(*initField.value);
                bool found = false;
                for (auto& f : st->fields) {
                    if (f.name == initField.name) {
                        Type* fType = resolveType(f.parsedType);
                        if (fType && initField.value->type && *initField.value->type != *fType) {
                            error(initField.value->loc, "Type mismatch for field '" + initField.name +
                                "'. Expected " + fType->toString() + ", got " + initField.value->type->toString());
                        }
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    error(initField.value->loc, "Field '" + initField.name + "' not found in struct '" + structInit->type->name + "'");
                }
            }
        }
        else if (auto flt = dynamic_cast<FloatLiteral*>(&expr)) {
            flt->type = &TypeF64;
        }
        else if (auto size = dynamic_cast<SizeofExpr*>(&expr)) {
            size->resolvedTargetType = resolveType(size->parsedTargetType);
            if (!size->resolvedTargetType) error(size->loc, "Unknown type in sizeof");
            size->type = &TypeU64;
        }
        else if (auto str = dynamic_cast<StringLiteral*>(&expr)) {
            ParsedType pt; pt.baseName = "u8"; pt.modifiers.push_back(TypeModifier::Ref);
            str->type = resolveType(pt);
        }
        else if (auto un = dynamic_cast<UnaryExpr*>(&expr)) {
            checkExpr(*un->operand);
            un->type = un->operand->type;
        }
        else if (auto addr = dynamic_cast<AddrOfExpr*>(&expr)) {
            checkExpr(*addr->operand);
            if (!addr->operand->type) {
                error(addr->loc, "Unknown type for address-of");
                addr->type = &TypeVoid;
                return;
            }
            ParsedType pt;
            pt.baseName = addr->operand->type->name;
            pt.modifiers.push_back(TypeModifier::Ref);
            addr->type = resolveType(pt);
        }
        else if (auto var = dynamic_cast<VarExpr*>(&expr)) {
            var->type = lookupVariable(var->name);

            if (!var->type && m_constants.count(var->name)) {
                var->type = resolveType(m_constants[var->name]->parsedType);
                return;
            }

            if (!var->type) {
                std::string mangled = var->name;
                size_t pos = 0;
                while ((pos = mangled.find("::", pos)) != std::string::npos) {
                    mangled.replace(pos, 2, "_");
                    pos += 1;
                }
                std::string baseName = var->name;
                auto rpos = baseName.rfind("::");
                if (rpos != std::string::npos) baseName = baseName.substr(rpos + 2);

                FunctionDecl* targetFn = nullptr;
                if (m_functions.count(var->name)) targetFn = m_functions[var->name];
                else if (m_functions.count(mangled)) targetFn = m_functions[mangled];
                else if (m_functions.count(baseName)) targetFn = m_functions[baseName];

                if (targetFn) {
                    Type* t = new Type{ ScalarType::FunctionPtr, targetFn->name + "_sig", 8 };
                    for (auto& p : targetFn->params) t->paramTypes.push_back(p.resolvedType);
                    t->returnType = targetFn->returnTypeResolved;
                    var->type = t;
                    return;
                }

                error(var->loc, "Undeclared variable: " + var->name);
                var->type = &TypeVoid;
                return;
            }

            for (Scope* s = m_currentScope; s; s = s->parent) {
                if (s->freedVars.count(var->name)) {
                    error(var->loc, "Use after free: Variable '" + var->name + "' was already freed.");
                    break;
                }
            }
        }
        else if (auto deref = dynamic_cast<DerefExpr*>(&expr)) {
            checkExpr(*deref->operand);
            if (!deref->operand->type->isPointer()) {
                error(deref->loc, "Cannot dereference non-pointer type.");
                deref->type = &TypeVoid;
                return;
            }
            deref->type = deref->operand->type->base;
        }
        else if (auto arr = dynamic_cast<ArrayAccessExpr*>(&expr)) {
            checkExpr(*arr->array);
            checkExpr(*arr->index);

            if (!arr->array->type || (!arr->array->type->isPointer() && !arr->array->type->isArray)) {
                error(arr->loc, "Array access requires array or pointer type.");
                arr->type = &TypeVoid;
                return;
            }
            arr->type = arr->array->type->base;
        }
        else if (auto alloc = dynamic_cast<AllocExpr*>(&expr)) {
            alloc->resolvedTargetType = resolveType(alloc->parsedTargetType);

            ParsedType pt = alloc->parsedTargetType;
            pt.modifiers.push_back(TypeModifier::Owner);
            alloc->type = resolveType(pt);

            if (!alloc->type || !alloc->resolvedTargetType) {
                error(alloc->loc, "Unknown type for allocation: " + alloc->parsedTargetType.toString());
                alloc->type = &TypeVoid;
                return;
            }

            std::string methodName = alloc->parsedTargetType.baseName + "_alloc";
            if (m_functions.count(methodName)) {
                alloc->initMethodName = methodName;
                FunctionDecl* fn = m_functions[methodName];

                if (alloc->args.size() + 1 != fn->params.size()) {
                    error(alloc->loc, "Argument count mismatch for " + methodName);
                }
                for (size_t i = 0; i < alloc->args.size(); ++i) {
                    checkExpr(*alloc->args[i]);
                }
            }
            else {
                for (auto& arg : alloc->args) checkExpr(*arg);
                if (!alloc->args.empty()) {
                    error(alloc->loc, "No alloc method found for " + alloc->parsedTargetType.baseName + " taking arguments.");
                }
            }
        }
        else if (auto mem = dynamic_cast<MemberExpr*>(&expr)) {
            checkExpr(*mem->object);

            Type* baseType = mem->object->type;

            if (baseType && baseType->isPointer()) {
                baseType = baseType->base;
            }

            if (baseType->scalar != ScalarType::Struct) {
                error(mem->loc, "Dot access on non-struct type.");
                mem->type = &TypeVoid;
                return;
            }

            StructDecl* st = m_structs[baseType->name];
            bool found = false;
            for (auto& f : st->fields) {
                if (f.name == mem->memberName) {
                    mem->type = resolveType(f.parsedType);
                    if (!mem->type) mem->type = &TypeVoid;
                    found = true;
                    break;
                }
            }
            if (!found) {
                error(mem->loc, "Field not found: " + mem->memberName);
                mem->type = &TypeVoid;
            }
        }
        else if (auto assign = dynamic_cast<AssignmentExpr*>(&expr)) {
            checkExpr(*assign->target);
            checkExpr(*assign->value);
            assign->type = assign->target->type;

            if (assign->op != TokenType::Equal && dynamic_cast<IntLiteral*>(assign->value.get())) {
                assign->value->type = assign->target->type;
            }

            bool isAlloc = dynamic_cast<AllocExpr*>(assign->value.get()) != nullptr;

            if (assign->target->type && assign->target->type->isPointer() && isAlloc) {

            }

            else if (assign->target->type && assign->value->type && *assign->target->type != *assign->value->type) {
                error(assign->loc, "Type mismatch in assignment. Cannot assign " +
                    assign->value->type->toString() + " to " + assign->target->type->toString());
            }
        }
        else if (auto bin = dynamic_cast<BinaryExpr*>(&expr)) {
            checkExpr(*bin->left);
            checkExpr(*bin->right);

            if (bin->op == TokenType::EqualEqual ||
                bin->op == TokenType::NotEqual ||
                bin->op == TokenType::LT ||
                bin->op == TokenType::GT ||
                bin->op == TokenType::LE ||
                bin->op == TokenType::GE) {

                bin->type = &TypeBool;
            }
            else {
                bin->type = bin->left->type;
            }
        }
        else if (auto call = dynamic_cast<CallExpr*>(&expr)) {
            if (auto mem = dynamic_cast<MemberExpr*>(call->callee.get())) {
                checkExpr(*mem->object);

                if (mem->object->type && (mem->object->type->scalar == ScalarType::Struct || mem->object->type->isPointer())) {
                    Type* baseType = mem->object->type;
                    bool isPtr = baseType->isPointer();
                    std::string structName = isPtr ? baseType->base->name : baseType->name;
                    std::string methodName = structName + "_" + mem->memberName;

                    if (m_functions.count(methodName)) {
                        FunctionDecl* fn = m_functions[methodName];

                        auto funcVar = std::make_unique<VarExpr>();
                        funcVar->loc = mem->loc;
                        funcVar->name = methodName;

                        std::unique_ptr<Expr> selfArg = std::move(mem->object);

                        if (!fn->params.empty()) {
                            Type* expectedSelfType = resolveType(fn->params[0].parsedType);
                            if (expectedSelfType && expectedSelfType->isPointer() && !isPtr) {
                                auto addrOf = std::make_unique<AddrOfExpr>();
                                addrOf->loc = selfArg->loc;
                                addrOf->operand = std::move(selfArg);
                                selfArg = std::move(addrOf);
                            }
                            else if (expectedSelfType && !expectedSelfType->isPointer() && isPtr) {
                                auto deref = std::make_unique<DerefExpr>();
                                deref->loc = selfArg->loc;
                                deref->operand = std::move(selfArg);
                                selfArg = std::move(deref);
                            }
                        }

                        call->args.insert(call->args.begin(), std::move(selfArg));
                        call->callee = std::move(funcVar);
                    }
                }
            }

            if (auto var = dynamic_cast<VarExpr*>(call->callee.get())) {
                bool isFunctionCall = false;

                std::string mangled = var->name;
                size_t pos = 0;
                while ((pos = mangled.find("::", pos)) != std::string::npos) {
                    mangled.replace(pos, 2, "_");
                    pos += 1;
                }
                std::string baseName = var->name;
                auto rpos = baseName.rfind("::");
                if (rpos != std::string::npos) baseName = baseName.substr(rpos + 2);

                FunctionDecl* targetFn = nullptr;
                if (m_functions.count(var->name)) targetFn = m_functions[var->name];
                else if (m_functions.count(mangled)) targetFn = m_functions[mangled];
                else if (m_functions.count(baseName)) targetFn = m_functions[baseName];

                if (targetFn) {
                    if (call->args.size() != targetFn->params.size()) error(call->loc, "Arg count mismatch");
                    for (auto& arg : call->args) checkExpr(*arg);
                    call->type = targetFn->returnTypeResolved;
                    isFunctionCall = true;
                }
                else if (!lookupVariable(var->name)) {
                    error(call->loc, "Undeclared function: " + var->name);
                    for (auto& arg : call->args) checkExpr(*arg);
                    call->type = &TypeVoid;
                    isFunctionCall = true;
                }

                if (isFunctionCall) {
                    if ((var->name == "free" || var->name.ends_with("::free") || var->name.ends_with("_free")) && call->args.size() == 1) {
                        Expr* argToFree = call->args[0].get();
                        while (auto castExpr = dynamic_cast<CastExpr*>(argToFree)) {
                            argToFree = castExpr->operand.get();
                        }

                        if (auto argVar = dynamic_cast<VarExpr*>(argToFree)) {
                            for (Scope* s = m_currentScope; s; s = s->parent) {
                                if (s->variables.count(argVar->name)) {
                                    s->freedVars.insert(argVar->name);
                                    break;
                                }
                            }
                        }
                    }
                    return;
                }
            }

            checkExpr(*call->callee);

            Type* calleeType = call->callee->type;
            if (!calleeType || calleeType->scalar != ScalarType::FunctionPtr) {
                error(call->loc, "Expression is not callable.");
                call->type = &TypeVoid;
                return;
            }

            if (call->args.size() != calleeType->paramTypes.size()) {
                error(call->loc, "Function pointer expects " + std::to_string(calleeType->paramTypes.size()) + " args.");
            }

            for (size_t i = 0; i < call->args.size(); ++i) {
                checkExpr(*call->args[i]);
            }
            call->type = calleeType->returnType;
        }
        else if (auto cast = dynamic_cast<CastExpr*>(&expr)) {
            checkExpr(*cast->operand);
            cast->targetType = resolveType(cast->parsedTargetType);
            if (!cast->targetType) {
                error(cast->loc, "Unknown cast type: " + cast->parsedTargetType.toString());
                expr.type = &TypeVoid;
                return;
            }

            if (cast->castKind == CastKind::Bits) {
                if (cast->targetType->sizeBytes != cast->operand->type->sizeBytes) {
                    error(cast->loc, "cast_bits requires same size types.");
                }
            }
            expr.type = cast->targetType;
        }
    }

    void Sema::enterScope() {
        Scope* s = new Scope();
        s->parent = m_currentScope;
        m_currentScope = s;
    }
    void Sema::exitScope() {
        Scope* old = m_currentScope;
        m_currentScope = old->parent;
        delete old;
    }
    bool Sema::declareVariable(const std::string& name, Type* type, SourceLoc loc, const std::set<std::string>& attrs) {
        m_currentScope->variables[name] = type;

        if (type->name.starts_with("owner") && attrs.find("nofree") == attrs.end()) {
            m_currentScope->ownedVars.insert(name);
        }
        return true;
    }
    bool Sema::declareVariable(const std::string& name, Type* type, SourceLoc loc) {
        m_currentScope->variables[name] = type;
        return true;
    }
    Type* Sema::lookupVariable(const std::string& name) {
        for (Scope* s = m_currentScope; s; s = s->parent)
            if (s->variables.count(name)) return s->variables[name];
        return nullptr;
    }
}