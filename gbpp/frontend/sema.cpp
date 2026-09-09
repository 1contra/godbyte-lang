#include "../include/sema.hpp"
#include <algorithm>
#include <iostream>
#include <set>

namespace gbpp {

    static int s_loopDepth = 0;

    thread_local std::vector<std::map<std::string, std::vector<Attribute>>> t_attrScopes;
    static void checkDeprecation(Sema* sema, const std::vector<Attribute>& attrs, SourceLoc loc, const std::string& name) {
        for (const auto& attr : attrs) {
            if (attr.kind == AttrKind::Deprecated) {
                std::string reason = "";
                std::string targetLink = "";
                if (!attr.args.empty()) {
                    reason = attr.args[0];
                    if (reason.length() >= 2 && (reason.front() == '"' || reason.front() == '\'')) {
                        reason = reason.substr(1, reason.length() - 2);
                    }

                    size_t linkStart = reason.find("@{link(");
                    if (linkStart != std::string::npos) {
                        size_t linkEnd = reason.find(")}", linkStart);
                        if (linkEnd != std::string::npos) {
                            targetLink = reason.substr(linkStart + 7, linkEnd - (linkStart + 7));
                            size_t eraseLen = linkEnd + 2 - linkStart;
                            if (linkStart + eraseLen < reason.length() && reason[linkStart + eraseLen] == ' ') {
                                eraseLen++;
                            }
                            reason.erase(linkStart, eraseLen);
                        }
                    }
                }

                std::string msg = "Use of deprecated item '" + name + "'";
                if (!reason.empty()) {
                    msg += ": " + reason;
                }

                std::string errStr = "Warning: Line " + std::to_string(loc.line) + ":" + std::to_string(loc.col) + " - " + msg;

                if (!targetLink.empty()) {
                    SourceLoc targetLoc = { "", 0, 0 };
                    if (sema->m_functions.count(targetLink)) targetLoc = sema->m_functions[targetLink]->loc;
                    else if (sema->m_generic_functions.count(targetLink)) targetLoc = sema->m_generic_functions[targetLink]->loc;
                    else if (sema->m_structs.count(targetLink)) targetLoc = sema->m_structs[targetLink]->loc;
                    else if (sema->m_generic_structs.count(targetLink)) targetLoc = sema->m_generic_structs[targetLink]->loc;
                    else if (sema->m_enums.count(targetLink)) targetLoc = sema->m_enums[targetLink]->loc;

                    if (targetLoc.line != 0) {
                        std::string relUri = targetLoc.filename;
                        if (!relUri.starts_with("file://")) {
                            std::string fixed = relUri;
                            std::replace(fixed.begin(), fixed.end(), '\\', '/');
                            if (fixed.length() >= 2 && fixed[1] == ':') fixed = "/" + fixed;
                            relUri = "file://" + fixed;
                        }
                        errStr += "\t[REL|" + relUri + "|" + std::to_string(targetLoc.line) + "|" + std::to_string(targetLoc.col) + "|" + targetLink + "]";
                    }
                }

                sema->errors.push_back(errStr);
            }
        }
    }

    Sema::Sema() {}

    void Sema::error(SourceLoc loc, const std::string& msg) {
        errors.push_back("Line " + std::to_string(loc.line) + ":" + std::to_string(loc.col) + " - " + msg);
    }

    bool Sema::analyze(Program& prog) {
        errors.clear();
        s_loopDepth = 0;
        t_attrScopes.clear();
        enterScope();

        for (auto& enm : prog.enums) m_enums[enm->name] = enm.get();
        for (auto& alias : prog.aliases) {
            m_aliases[alias->name] = alias->targetType;
        }

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
            if (fn->isOperator) {
                m_structOperators[fn->parentStructName][fn->operatorKind] = fn.get();
            }
        }

        size_t sigIdx = 0;
        while (sigIdx < m_pending_function_checks.size()) {
            FunctionDecl* fn = m_pending_function_checks[sigIdx];
            if (!fn->signatureType) {
                fn->signatureType = std::make_unique<Type>(Type{ ScalarType::FunctionPtr, fn->name + "_sig", 8 });
                for (auto& param : fn->params) {
                    param.resolvedType = resolveType(param.parsedType);
                    fn->signatureType->paramTypes.push_back(param.resolvedType);
                }
                fn->returnTypeResolved = resolveType(fn->returnType);
                if (!fn->returnTypeResolved) fn->returnTypeResolved = &TypeVoid;
            }
            sigIdx++;
        }

        for (auto& v : prog.globalVars) {
            m_globalVars[v->name] = v.get();
            checkStmt(*v);
        }

        for (auto& st : prog.structs) {
            if (st->genericParams.empty()) {
                int runningOffset = 0;
                for (auto& field : st->fields) {
                    if (field.offset > 0 && field.offset < runningOffset) {
                        //error(st->loc, "Struct field overlap detected in '" + st->name + "'.");
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
        t_attrScopes.clear();
        enterScope();

        for (auto prog : progs) {
            for (auto& v : prog->globalVars) {
                m_globalVars[v->name] = v.get();
            }
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
                if (fn->isOperator) {
                    m_structOperators[fn->parentStructName][fn->operatorKind] = fn.get();
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

        size_t sigIdx = 0;
        while (sigIdx < m_pending_function_checks.size()) {
            FunctionDecl* fn = m_pending_function_checks[sigIdx];
            if (!fn->signatureType) {
                fn->signatureType = std::make_unique<Type>(
                    Type{ ScalarType::FunctionPtr, fn->name + "_sig", 8 }
                );

                for (auto& param : fn->params) {
                    param.resolvedType = resolveType(param.parsedType);
                    fn->signatureType->paramTypes.push_back(param.resolvedType);
                }

                fn->returnTypeResolved = resolveType(fn->returnType);
                if (!fn->returnTypeResolved)
                    fn->returnTypeResolved = &TypeVoid;
            }

            sigIdx++;
        }

        for (auto prog : progs) {
            for (auto& v : prog->globalVars) {
                checkStmt(*v);
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
            actual.isUnion = target.isUnion;
            actual.unionTypes = target.unionTypes;

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
            t->isVariadicFunc = actual.isVariadicFunc;
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
                    mangledName += "$" + argType->name;
                }
                else {
                    mangledName += "$" + arg.baseName;
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

        if (actual.isUnion) {
            baseType = new Type{ ScalarType::Union, actual.toString(), 0 };
            int maxSize = 0;
            for (const auto& ut : actual.unionTypes) {
                Type* resolvedUt = resolveType(ut);
                if (resolvedUt) {
                    baseType->unionTypes.push_back(resolvedUt);
                    if (resolvedUt->sizeBytes > maxSize) maxSize = resolvedUt->sizeBytes;
                }
            }
            baseType->sizeBytes = std::max(8, maxSize + 8);
        }
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

        bool isVolatile = actual.hasModifier(TypeModifier::Volatile);
        bool isConst = actual.hasModifier(TypeModifier::Const);

        baseType = new Type(*baseType);
        baseType->isVolatile = isVolatile;
        baseType->isConst = isConst;

        if (actual.isArray) {
            if (actual.arraySizeExpr.empty()) {
                baseType = new Type{
                    ScalarType::Pointer,
                    actual.toString(),
                    8,
                    false, false, true,
                    baseType,
                    isVolatile
                };
            }
            else {
                int arrSize = 0;
                bool isDynamic = false;
                try {
                    arrSize = std::stoi(actual.arraySizeExpr);
                }
                catch (...) {
                    if (m_globalVars.count(actual.arraySizeExpr) && m_globalVars[actual.arraySizeExpr]->initializer) {
                        if (auto lit = dynamic_cast<IntLiteral*>(m_globalVars[actual.arraySizeExpr]->initializer.get())) {
                            arrSize = std::stoi(lit->value);
                        }
                        else {
                            errors.push_back("Semantic Error: Array size constant '" + actual.arraySizeExpr + "' must be initialized with an integer literal.");
                            isDynamic = true;
                        }
                    }
                    else {
                        errors.push_back("Semantic Error: Invalid array size expression '" + actual.arraySizeExpr + "'. Must be a constant integer.");
                        isDynamic = true;
                    }
                }
                baseType = new Type{
                    ScalarType::Struct,
                    actual.toString(),
                    isDynamic ? 0 : (baseType->sizeBytes * arrSize),
                    false, false, true,
                    baseType,
                    isVolatile
                };
            }
        }

        for (auto it = actual.modifiers.rbegin(); it != actual.modifiers.rend(); ++it) {
            if (*it == TypeModifier::Volatile || *it == TypeModifier::Const) continue;

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
        if (pt.isUnion) {
            for (auto& u : pt.unionTypes) {
                u = substituteType(u, subs);
            }
        }
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
            auto res = std::make_unique<VarExpr>(); res->loc = v->loc; res->name = v->name;
            for (auto& arg : v->genericArgs) res->genericArgs.push_back(substituteType(arg, subs));
            return res;
        }
        if (auto d = dynamic_cast<DerefExpr*>(e)) {
            auto res = std::make_unique<DerefExpr>(); res->loc = d->loc; res->operand = cloneExpr(d->operand.get(), subs); return res;
        }
        if (auto a = dynamic_cast<ArrayAccessExpr*>(e)) {
            auto res = std::make_unique<ArrayAccessExpr>(); res->loc = a->loc; res->array = cloneExpr(a->array.get(), subs); res->index = cloneExpr(a->index.get(), subs);
            res->overloadedCall = cloneExpr(a->overloadedCall.get(), subs);
            return res;
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
        if (auto b = dynamic_cast<BuiltinCallExpr*>(e)) {
            auto res = std::make_unique<BuiltinCallExpr>();
            res->loc = b->loc;
            res->builtinType = b->builtinType;

            for (auto& arg : b->args) {
                res->args.push_back(cloneExpr(arg.get(), subs));
            }

            return res;
        }
        if (auto c = dynamic_cast<CastExpr*>(e)) {
            auto res = std::make_unique<CastExpr>(); res->loc = c->loc; res->castKind = c->castKind; res->parsedTargetType = substituteType(c->parsedTargetType, subs); res->operand = cloneExpr(c->operand.get(), subs); return res;
        }
        if (auto m = dynamic_cast<MemberExpr*>(e)) {
            auto res = std::make_unique<MemberExpr>(); res->loc = m->loc; res->object = cloneExpr(m->object.get(), subs); res->memberName = m->memberName; return res;
        }
        if (auto a = dynamic_cast<AssignmentExpr*>(e)) {
            auto res = std::make_unique<AssignmentExpr>(); res->loc = a->loc; res->op = a->op; res->target = cloneExpr(a->target.get(), subs); res->value = cloneExpr(a->value.get(), subs); return res;
        }
        if (auto b = dynamic_cast<BinaryExpr*>(e)) {
            auto res = std::make_unique<BinaryExpr>(); res->loc = b->loc; res->op = b->op; res->left = cloneExpr(b->left.get(), subs); res->right = cloneExpr(b->right.get(), subs);
            res->overloadedCall = cloneExpr(b->overloadedCall.get(), subs);
            return res;
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
        if (auto hasMeth = dynamic_cast<CompilerHasMethodExpr*>(e)) {
            auto res = std::make_unique<CompilerHasMethodExpr>(); res->loc = hasMeth->loc;
            res->parsedTargetType = substituteType(hasMeth->parsedTargetType, subs);
            res->methodName = hasMeth->methodName; return res;
        }
        if (auto al = dynamic_cast<AlignofExpr*>(e)) {
            auto res = std::make_unique<AlignofExpr>(); res->loc = al->loc;
            res->parsedTargetType = substituteType(al->parsedTargetType, subs); return res;
        }
        if (auto exp = dynamic_cast<ExpandExpr*>(e)) {
            auto res = std::make_unique<ExpandExpr>(); res->loc = exp->loc;
            res->operand = cloneExpr(exp->operand.get(), subs); return res;
        }
        if (auto le = dynamic_cast<LockExpr*>(e)) {
            auto res = std::make_unique<LockExpr>(); res->loc = le->loc; res->operand = cloneExpr(le->operand.get(), subs); return res;
        }
        return nullptr;
    }

    bool Sema::evaluateComptimeBool(Expr* expr) {
        if (auto lit = dynamic_cast<IntLiteral*>(expr)) {
            return lit->value != "0" && lit->value != "0u8";
        }
        if (auto hasMeth = dynamic_cast<CompilerHasMethodExpr*>(expr)) {
            return hasMeth->resultValue;
        }
        error(expr->loc, "Condition is not a compile-time evaluable constant.");
        return false;
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
        if (auto cIf = dynamic_cast<ComptimeIfStmt*>(s)) {
            auto res = std::make_unique<ComptimeIfStmt>(); res->loc = cIf->loc;
            res->condition = cloneExpr(cIf->condition.get(), subs);
            res->thenBranch = cloneStmt(cIf->thenBranch.get(), subs);
            res->elseBranch = cloneStmt(cIf->elseBranch.get(), subs); return res;
        }
        return nullptr;
    }

    FunctionDecl* Sema::instantiateFunction(FunctionDecl* tmpl, const std::vector<ParsedType>& args, const std::string& mangledName) {
        std::unordered_map<std::string, std::string> substitutions;
        for (size_t i = 0; i < tmpl->genericParams.size(); ++i) {
            if (i < args.size()) {
                Type* argType = resolveType(args[i]);
                substitutions[tmpl->genericParams[i].name] = argType ? argType->name : args[i].baseName;
            }
            else if (tmpl->genericParams[i].isVariadic) {
                substitutions[tmpl->genericParams[i].name] = "void";
            }
        }

        auto inst = std::make_unique<FunctionDecl>();
        inst->loc = tmpl->loc;
        inst->name = mangledName;
        inst->attributes = tmpl->attributes;
        inst->genericParams = tmpl->genericParams;

        inst->isOperator = tmpl->isOperator;
        inst->operatorKind = tmpl->operatorKind;
        inst->parentStructName = tmpl->parentStructName;
        inst->isVariadic = tmpl->isVariadic;

        for (const auto& p : tmpl->params) {
            inst->params.push_back({ p.name, substituteType(p.parsedType, substitutions), nullptr, p.attributes });
        }
        inst->returnType = substituteType(tmpl->returnType, substitutions);

        if (tmpl->body) {
            inst->body = std::unique_ptr<BlockStmt>(static_cast<BlockStmt*>(cloneStmt(tmpl->body.get(), substitutions).release()));
        }

        FunctionDecl* rawPtr = inst.get();
        m_functions[mangledName] = rawPtr;
        m_instantiated_functions.push_back(std::move(inst));

        if (!rawPtr->signatureType) {
            rawPtr->signatureType = std::make_unique<Type>(Type{ ScalarType::FunctionPtr, rawPtr->name + "_sig", 8 });
            for (auto& param : rawPtr->params) {
                param.resolvedType = resolveType(param.parsedType);
                rawPtr->signatureType->paramTypes.push_back(param.resolvedType);
            }
            rawPtr->returnTypeResolved = resolveType(rawPtr->returnType);
            if (!rawPtr->returnTypeResolved) rawPtr->returnTypeResolved = &TypeVoid;
        }

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
            inst->fields.push_back({ f.name, subbedType, offset, f.attributes });
            runningOffset = std::max(runningOffset, offset) + fieldSize;
        }

        m_instantiated_structs.push_back(std::move(inst));

        std::string prefixC = tmpl->name + "::";
        for (auto& [gName, gFn] : m_generic_functions) {
            if (gName.starts_with(prefixC)) {
                std::string methodName = gName.substr(prefixC.length());
                std::string mangledFnName = mangledName + "::" + methodName;
                if (!m_functions.count(mangledFnName)) {
                    FunctionDecl* instFn = instantiateFunction(gFn, args, mangledFnName);

                    if (instFn->isOperator) {
                        instFn->parentStructName = mangledName;
                        m_structOperators[mangledName][instFn->operatorKind] = instFn;
                    }
                }
            }
        }

        return rawPtr;
    }

    void Sema::checkFunction(FunctionDecl& fn) {
        if (!fn.signatureType) {
            fn.signatureType = std::make_unique<Type>(Type{ ScalarType::FunctionPtr, fn.name + "_sig", 8 });
            fn.signatureType->isVariadicFunc = fn.isVariadic;
            for (auto& param : fn.params) {
                param.resolvedType = resolveType(param.parsedType);
                fn.signatureType->paramTypes.push_back(param.resolvedType);
            }
            fn.returnTypeResolved = resolveType(fn.returnType);
            if (!fn.returnTypeResolved) fn.returnTypeResolved = &TypeVoid;
        }

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
        else if (!hasAttribute(fn.attributes, AttrKind::Extern)) {
            error(fn.loc, "Function '" + fn.name + "' has no body. To declare an external interface, use [[@extern]]");
        }
        exitScope();

        m_currentFunctionReturnType = nullptr;
    }

    void Sema::checkBlock(BlockStmt& block) {
        enterScope();
        for (auto& stmt : block.statements) checkStmt(*stmt);

        for (const auto& owner : m_currentScope->ownedVars) {
            bool freed = m_currentScope->freedVars.find(owner) != m_currentScope->freedVars.end();
            bool moved = m_currentScope->movedVars.find(owner) != m_currentScope->movedVars.end();
            if (!freed && !moved) {
                error(block.loc, "Memory leak detected. Variable '" + owner +
                    "' is a 'heap' type but was never freed or moved.");
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
                if (d->resolvedType && d->initializer->type && *d->resolvedType != *d->initializer->type) {
                    error(d->loc, "Type mismatch in declaration of '" + d->name +
                        "'. Expected " + d->resolvedType->toString() + ", got " + d->initializer->type->toString());
                }
            }
            declareVariable(d->name, d->resolvedType, d->loc, d->attributes);
        }
        else if (auto asmStmt = dynamic_cast<AsmStmt*>(&stmt)) {
            // fuck you
        }
        else if (auto b = dynamic_cast<BreakStmt*>(&stmt)) {
            if (s_loopDepth == 0) error(b->loc, "'break' statement not within a loop.");
        }
        else if (auto r = dynamic_cast<ReturnStmt*>(&stmt)) {
            if (r->value) {
                checkExpr(*r->value);

                if (!r->value->type) r->value->type = &TypeVoid;

                if (m_currentFunctionReturnType && m_currentFunctionReturnType->name.starts_with("heap ")) {
                    Expr* retVal = r->value.get();

                    while (auto castExpr = dynamic_cast<CastExpr*>(retVal)) {
                        retVal = castExpr->operand.get();
                    }

                    if (auto var = dynamic_cast<VarExpr*>(retVal)) {
                        for (Scope* s = m_currentScope; s; s = s->parent) {
                            if (s->variables.count(var->name)) {
                                s->movedVars.insert(var->name);
                                break;
                            }
                        }
                    }
                }

                if (m_currentFunctionReturnType->scalar == ScalarType::Void) {
                    error(r->loc, "Function returning 'void' cannot return a value.");
                }
                else {
                    if (*r->value->type != *m_currentFunctionReturnType) {
                        error(r->loc, "Return type mismatch. Expected " +
                            m_currentFunctionReturnType->toString() + ", got " + r->value->type->toString());
                    }
                }
            }
            else {
                if (m_currentFunctionReturnType->scalar != ScalarType::Void) {
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
        else if (auto blk = dynamic_cast<BlockStmt*>(&stmt)) {
            checkBlock(*blk);
        }
        else if (auto cIf = dynamic_cast<ComptimeIfStmt*>(&stmt)) {
            checkExpr(*cIf->condition);
            bool condValue = evaluateComptimeBool(cIf->condition.get());
            if (condValue) {
                if (cIf->thenBranch) checkStmt(*cIf->thenBranch);
                cIf->elseBranch.reset();
            }
            else {
                if (cIf->elseBranch) checkStmt(*cIf->elseBranch);
                cIf->thenBranch.reset();
            }
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
            checkDeprecation(this, enm->attributes, enumAcc->loc, enm->name);
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

            if (!m_structs.count(structInit->type->name)) {
                error(structInit->loc, "Struct definition not fully resolved: " + structInit->type->name);
                structInit->type = &TypeVoid;
                return;
            }

            StructDecl* st = m_structs[structInit->type->name];
            checkDeprecation(this, st->attributes, structInit->loc, st->name);

            for (auto& initField : structInit->fields) {
                checkExpr(*initField.value);
                if (!initField.value->type) initField.value->type = &TypeVoid;
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
            if (!un->operand->type) un->operand->type = &TypeVoid;
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
            std::string baseTypeStr = addr->operand->type->name;

            if (baseTypeStr.starts_with("heap ")) {
                pt.baseName = baseTypeStr.substr(5);
                pt.modifiers.push_back(TypeModifier::Ref);
            }
            else {
                pt.baseName = baseTypeStr;
                pt.modifiers.push_back(TypeModifier::Ref);
            }

            if (addr->operand->type->isVolatile) pt.modifiers.push_back(TypeModifier::Volatile);
            if (addr->operand->type->isConst) pt.modifiers.push_back(TypeModifier::Const);

            addr->type = resolveType(pt);
        }
        else if (auto var = dynamic_cast<VarExpr*>(&expr)) {
            if (!var->genericArgs.empty()) {
                std::string mangledName = var->name;
                for (const auto& arg : var->genericArgs) {
                    Type* argType = resolveType(arg);
                    mangledName += "$" + (argType ? argType->name : arg.baseName);
                }

                if (m_generic_functions.count(var->name)) {
                    if (!m_functions.count(mangledName)) {
                        instantiateFunction(m_generic_functions[var->name], var->genericArgs, mangledName);
                    }
                    var->name = mangledName;
                }
            }

            std::vector<Attribute> attrs;
            bool foundAttrs = false;
            for (auto it = t_attrScopes.rbegin(); it != t_attrScopes.rend(); ++it) {
                if (it->count(var->name)) { attrs = it->at(var->name); foundAttrs = true; break; }
            }
            if (!foundAttrs) {
                if (m_globalVars.count(var->name)) attrs = m_globalVars[var->name]->attributes;
                else if (m_functions.count(var->name)) attrs = m_functions[var->name]->attributes;
                else if (m_generic_functions.count(var->name)) attrs = m_generic_functions[var->name]->attributes;
            }
            checkDeprecation(this, attrs, var->loc, var->name);

            var->type = lookupVariable(var->name);

            for (Scope* s = m_currentScope; s; s = s->parent) {
                if (s->movedVars.count(var->name)) {
                    error(var->loc, "Use of moved heap variable: '" + var->name + "'");
                    break;
                }
            }

            if (!var->type) {
                FunctionDecl* targetFn = nullptr;
                if (m_functions.count(var->name)) {
                    targetFn = m_functions[var->name];
                }

                if (targetFn) {
                    Type* t = new Type{ ScalarType::FunctionPtr, targetFn->name + "_sig", 8 };
                    for (auto& p : targetFn->params) t->paramTypes.push_back(p.resolvedType);
                    t->returnType = targetFn->returnTypeResolved;
                    var->type = t;
                    if (hasAttribute(targetFn->attributes, AttrKind::Extern)) {
                        size_t p = targetFn->name.rfind("::");
                        var->name = (p != std::string::npos) ? targetFn->name.substr(p + 2) : targetFn->name;
                    }
                    else {
                        var->name = targetFn->name;
                    }
                    return;
                }
                error(var->loc, "Undeclared variable: " + var->name);
                var->type = &TypeVoid;
                return;
            }
        }
        else if (auto deref = dynamic_cast<DerefExpr*>(&expr)) {
            checkExpr(*deref->operand);
            if (!deref->operand->type) deref->operand->type = &TypeVoid;

            if (!deref->operand->type->isPointer()) {
                error(deref->loc, "Cannot dereference non-pointer type.");
                deref->type = &TypeVoid;
                return;
            }
            deref->type = deref->operand->type->base;
            if (!deref->type) deref->type = &TypeVoid;
        }
        else if (auto arr = dynamic_cast<ArrayAccessExpr*>(&expr)) {
            checkExpr(*arr->array);
            checkExpr(*arr->index);

            Type* baseType = arr->array->type;
            bool isPtr = false;
            if (baseType && baseType->isPointer()) {
                baseType = baseType->base;
                isPtr = true;
            }

            if (baseType && baseType->scalar == ScalarType::Struct) {
                FunctionDecl* fn = nullptr;

                if (m_structOperators.count(baseType->name) && m_structOperators[baseType->name].count(TokenType::LBracket)) {
                    fn = m_structOperators[baseType->name][TokenType::LBracket];
                }

                if (fn) {
                    auto call = std::make_unique<CallExpr>();
                    call->loc = arr->loc;
                    auto calleeVar = std::make_unique<VarExpr>();
                    calleeVar->loc = arr->loc;
                    calleeVar->name = fn->name;
                    call->callee = std::move(calleeVar);

                    std::unique_ptr<Expr> selfArg = cloneExpr(arr->array.get(), {});
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
                    call->args.push_back(std::move(selfArg));
                    call->args.push_back(cloneExpr(arr->index.get(), {}));

                    checkExpr(*call);
                    arr->type = call->type;
                    if (arr->type && arr->type->isPointer()) {
                        arr->type = arr->type->base;
                    }
                    arr->overloadedCall = std::move(call);
                    return;
                }
            }

            if (!arr->array->type || (!arr->array->type->isArray && !arr->array->type->isPointer())) {
                error(arr->loc, "Cannot index into a non-array/pointer type");
                arr->type = &TypeVoid;
                return;
            }
            arr->type = arr->array->type->base;
            if (!arr->type) {
                arr->type = &TypeVoid;
            }
            }
        else if (auto memExpr = dynamic_cast<MemberExpr*>(&expr)) {
            checkExpr(*memExpr->object);

            if (!memExpr->object->type) {
                memExpr->type = &TypeVoid;
                return;
            }

            Type* objType = memExpr->object->type;
            while (objType->isPointer()) {
                objType = objType->base;
            }

            if (objType->scalar != ScalarType::Struct) {
                error(memExpr->loc, "Cannot access member of non-struct type");
                memExpr->type = &TypeVoid;
                return;
            }

            if (!m_structs.count(objType->name)) {
                error(memExpr->loc, "Unknown struct type: " + objType->name);
                memExpr->type = &TypeVoid;
                return;
            }

            StructDecl* st = m_structs[objType->name];
            bool found = false;
            for (auto& f : st->fields) {
                if (f.name == memExpr->memberName) {
                    checkDeprecation(this, f.attributes, memExpr->loc, f.name);
                    memExpr->type = resolveType(f.parsedType);
                    found = true;
                    break;
                }
            }

            if (!found) {
                error(memExpr->loc, "Struct '" + objType->name + "' has no member '" + memExpr->memberName + "'");
                memExpr->type = &TypeVoid;
            }
        }
        else if (auto assign = dynamic_cast<AssignmentExpr*>(&expr)) {
            checkExpr(*assign->target);
            checkExpr(*assign->value);

            if (!assign->target->type) assign->target->type = &TypeVoid;
            if (!assign->value->type) assign->value->type = &TypeVoid;

            if (assign->target->type && assign->target->type->isConst) {
                error(assign->loc, "Cannot assign to a const variable.");
            }

            assign->type = assign->target->type;

            if (assign->op == TokenType::Equal) {
                if (assign->target->type && assign->target->type->name.starts_with("heap ")) {
                    if (auto rhsVar = dynamic_cast<VarExpr*>(assign->value.get())) {
                        for (Scope* s = m_currentScope; s; s = s->parent) {
                            if (s->variables.count(rhsVar->name)) {
                                s->movedVars.insert(rhsVar->name);
                                break;
                            }
                        }
                    }
                }
            }

            if (assign->op != TokenType::Equal && dynamic_cast<IntLiteral*>(assign->value.get())) {
                Type* t = assign->target->type;
                if (t && (t->isInteger() || t->isFloatingPoint() || t->isPointer())) {
                    assign->value->type = t;
                }
                else {
                    error(assign->loc, "Cannot perform arithmetic assignment on non-numeric type: " + (t ? t->toString() : "unknown"));
                }


                if (assign->target->type && assign->target->type->isPointer()) {

                }

                if (assign->target->type && assign->value->type && *assign->target->type != *assign->value->type) {
                    error(assign->loc, "Type mismatch in assignment. Cannot assign " +
                        assign->value->type->toString() + " to " + assign->target->type->toString());
                }
            }
        }
        else if (auto bin = dynamic_cast<BinaryExpr*>(&expr)) {
            checkExpr(*bin->left);
            checkExpr(*bin->right);

            Type* leftType = bin->left->type ? bin->left->type : &TypeVoid;
            Type* rightType = bin->right->type ? bin->right->type : &TypeVoid;

            Type* baseType = leftType;
            bool isPtr = false;
            if (baseType && baseType->isPointer()) {
                baseType = baseType->base;
                isPtr = true;
            }

            if (baseType && baseType->scalar == ScalarType::Struct) {
                FunctionDecl* fn = nullptr;

                if (m_structOperators.count(baseType->name) && m_structOperators[baseType->name].count(bin->op)) {
                    fn = m_structOperators[baseType->name][bin->op];
                }

                if (fn) {
                    auto call = std::make_unique<CallExpr>();
                    call->loc = bin->loc;
                    auto calleeVar = std::make_unique<VarExpr>();
                    calleeVar->loc = bin->loc;
                    calleeVar->name = fn->name;
                    call->callee = std::move(calleeVar);

                    std::unique_ptr<Expr> selfArg = cloneExpr(bin->left.get(), {});
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
                    call->args.push_back(std::move(selfArg));
                    call->args.push_back(cloneExpr(bin->right.get(), {}));

                    checkExpr(*call);
                    bin->type = call->type;
                    bin->overloadedCall = std::move(call);
                    return;
                }
                
            }

            auto isNumeric = [](Type* t) {
                if (!t) return false;
                return t->isInteger() || t->isFloatingPoint();
                };

            auto isPrimitive = [](Type* t) {
                if (!t) return false;
                return t->isInteger() || t->isFloatingPoint() || t->isPointer() || t->name == "null" || t->scalar == ScalarType::FunctionPtr;
            };

            bool leftNull = (leftType->name == "null");
            bool rightNull = (rightType->name == "null");

            if (bin->op == TokenType::EqualEqual ||
                bin->op == TokenType::NotEqual ||
                bin->op == TokenType::LT ||
                bin->op == TokenType::GT ||
                bin->op == TokenType::LE ||
                bin->op == TokenType::GE) {

                if (bin->op == TokenType::EqualEqual || bin->op == TokenType::NotEqual) {
                    if ((leftNull && isNumeric(rightType)) || (isNumeric(leftType) && rightNull)) {
                        error(bin->loc, "Type mismatch: 'null' is its own distinct type and cannot be compared to a numeric value.");
                    }
                    else if ((leftNull && leftType->isPointer()) || (rightNull && rightType->isPointer())) {
                        bin->type = &TypeBool;
                    }
                    else if ((leftType->isPointer() && isNumeric(rightType)) || (isNumeric(leftType) && rightType->isPointer())) {
                        bin->type = &TypeBool;
                    }
                    else if (*leftType != *rightType) {
                        if (!(isPrimitive(leftType) && isPrimitive(rightType))) {
                            error(bin->loc, "Type mismatch: cannot compare '" +
                                leftType->toString() + "' and '" + rightType->toString() + "'");
                        }
                    }
                }
                else {
                    if (leftNull || rightNull) {
                        error(bin->loc, "Type mismatch: 'null' cannot be relationally compared.");
                    }
                    else if (!isNumeric(leftType) && !leftType->isPointer()) {
                        error(bin->loc, "Relational operators require numeric or pointer types, got '" + leftType->toString() + "'");
                    }
                }
                bin->type = &TypeBool;
            }
            else if (bin->op == TokenType::AmpAmp || bin->op == TokenType::PipePipe) {
                if (*leftType != *rightType) {
                    if (!(isPrimitive(leftType) && isPrimitive(rightType))) {
                        error(bin->loc, "Type mismatch in logical expression.");
                    }
                }
                bin->type = &TypeBool;
            }
            else {
                if (leftType->isPointer() && rightType->isInteger() &&
                    (bin->op == TokenType::Plus || bin->op == TokenType::Minus)) {
                    bin->type = leftType;
                }
                else if (*leftType != *rightType) {
                    if (!(isNumeric(leftType) && isNumeric(rightType))) {
                        error(bin->loc, "Type mismatch in binary expression. Cannot operate on '" +
                            leftType->toString() + "' and '" + rightType->toString() + "'");
                    }
                    bin->type = leftType;
                }
                else {
                    bin->type = leftType;
                }

                if (bin->op == TokenType::Pipe || bin->op == TokenType::Ampersand ||
                    bin->op == TokenType::Caret || bin->op == TokenType::ShiftLeft ||
                    bin->op == TokenType::ShiftRight || bin->op == TokenType::Percent) {

                    if (!leftType->isInteger() || !rightType->isInteger()) {
                        error(bin->loc, "Bitwise and modulo operators require integer types.");
                    }
                }
                else if (bin->op == TokenType::Plus || bin->op == TokenType::Minus ||
                    bin->op == TokenType::Star || bin->op == TokenType::Slash) {

                    if (!isNumeric(leftType) && !(leftType->isPointer() && rightType->isInteger() && (bin->op == TokenType::Plus || bin->op == TokenType::Minus))) {
                        error(bin->loc, "Arithmetic operators require numeric types.");
                    }
                }
            }
        }
        else if (auto call = dynamic_cast<CallExpr*>(&expr)) {
            for (auto& arg : call->args) {
                if (!arg->type) {
                    checkExpr(*arg);
                    if (!arg->type) arg->type = &TypeVoid;
                }
            }

            if (auto mem = dynamic_cast<MemberExpr*>(call->callee.get())) {
                checkExpr(*mem->object);
                if (mem->object->type && (mem->object->type->scalar == ScalarType::Struct || mem->object->type->isPointer())) {
                    Type* baseType = mem->object->type;
                    bool isPtr = baseType->isPointer();
                    std::string structName = isPtr ? baseType->base->name : baseType->name;
                    std::string expectedMethodName = structName + "::" + mem->memberName;
                    FunctionDecl* fn = nullptr;
                    if (m_functions.count(expectedMethodName)) fn = m_functions[expectedMethodName];
                    else if (m_generic_functions.count(expectedMethodName)) fn = m_generic_functions[expectedMethodName];
                    if (fn) {
                        auto funcVar = std::make_unique<VarExpr>();
                        funcVar->loc = mem->loc;
                        funcVar->name = expectedMethodName;
                        std::unique_ptr<Expr> selfArg = std::move(mem->object);
                        if (!fn->params.empty()) {
                            Type* expectedSelfType = resolveType(fn->params[0].parsedType);
                            if (expectedSelfType && expectedSelfType->isPointer() && !isPtr) {
                                auto addrOf = std::make_unique<AddrOfExpr>();
                                addrOf->loc = selfArg->loc;
                                addrOf->operand = std::move(selfArg);
                                selfArg = std::move(addrOf);
                                checkExpr(*selfArg);
                            }
                            else if (expectedSelfType && !expectedSelfType->isPointer() && isPtr) {
                                auto deref = std::make_unique<DerefExpr>();
                                deref->loc = selfArg->loc;
                                deref->operand = std::move(selfArg);
                                selfArg = std::move(deref);
                                checkExpr(*selfArg);
                            }
                        }
                        call->args.insert(call->args.begin(), std::move(selfArg));
                        call->callee = std::move(funcVar);
                    }
                }
            }

            if (auto var = dynamic_cast<VarExpr*>(call->callee.get())) {
                if (m_generic_functions.count(var->name)) {
                    FunctionDecl* tmpl = m_generic_functions[var->name];

                    while (var->genericArgs.size() < tmpl->genericParams.size()) {
                        size_t genIdx = var->genericArgs.size();
                        std::string genericName = tmpl->genericParams[genIdx].name;

                        int paramIdx = -1;
                        for (size_t p = 0; p < tmpl->params.size(); ++p) {
                            if (tmpl->params[p].parsedType.baseName == genericName) {
                                paramIdx = (int)p;
                                break;
                            }
                        }

                        if (paramIdx != -1 && paramIdx < (int)call->args.size() && call->args[paramIdx]->type) {
                            ParsedType deduced;
                            deduced.baseName = call->args[paramIdx]->type->name;
                            if (call->args[paramIdx]->type->isPointer()) {
                                deduced.baseName = call->args[paramIdx]->type->base->name;
                                deduced.modifiers.push_back(TypeModifier::Ref);
                            }
                            var->genericArgs.push_back(deduced);
                        }
                        else {
                            ParsedType voidPt; voidPt.baseName = "void";
                            var->genericArgs.push_back(voidPt);
                        }
                    }

                    std::string mangledName = var->name;
                    for (const auto& arg : var->genericArgs) {
                        Type* argType = resolveType(arg);
                        mangledName += "$" + (argType ? argType->name : arg.baseName);
                    }
                    if (!m_functions.count(mangledName)) {
                        instantiateFunction(tmpl, var->genericArgs, mangledName);
                    }
                    var->name = mangledName;
                }

                bool isFunctionCall = false;
                FunctionDecl* targetFn = nullptr;
                if (m_functions.count(var->name)) {
                    targetFn = m_functions[var->name];
                }

                if (targetFn) {
                    checkDeprecation(this, targetFn->attributes, var->loc, targetFn->name);
                    bool isVariadic = targetFn->isVariadic;
                    for (const auto& gp : targetFn->genericParams) {
                        if (gp.isVariadic) isVariadic = true;
                    }
                    bool hasExpand = false;
                    for (const auto& arg : call->args) {
                        if (dynamic_cast<ExpandExpr*>(arg.get())) hasExpand = true;
                    }
                    if (!isVariadic && !hasExpand && call->args.size() != targetFn->params.size()) {
                        error(call->loc, "Arg count mismatch for function '" + targetFn->name + "'");
                    }
                    if (isVariadic && !hasExpand && call->args.size() < targetFn->params.size() - 1) {
                        error(call->loc, "Not enough arguments for variadic function '" + targetFn->name + "'");
                    }
                    for (size_t i = 0; i < call->args.size(); ++i) {
                        auto& arg = call->args[i];
                        if (!hasExpand && i < targetFn->params.size()) {
                            if (targetFn->isVariadic && i >= targetFn->params.size() - 1) continue;
                            Type* expectedParam = targetFn->params[i].resolvedType;
                            if (expectedParam && expectedParam->name.starts_with("heap ")) {
                                if (auto argVar = dynamic_cast<VarExpr*>(arg.get())) {
                                    for (Scope* s = m_currentScope; s; s = s->parent) {
                                        if (s->variables.count(argVar->name)) {
                                            s->movedVars.insert(argVar->name);
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    call->type = targetFn->returnTypeResolved ? targetFn->returnTypeResolved : &TypeVoid;
                    isFunctionCall = true;
                    if (hasAttribute(targetFn->attributes, AttrKind::Extern)) {
                        size_t p = targetFn->name.rfind("::");
                        var->name = (p != std::string::npos) ? targetFn->name.substr(p + 2) : targetFn->name;
                    }
                    else {
                        var->name = targetFn->name;
                    }
                }
                else if (!lookupVariable(var->name)) {
                    error(call->loc, "Undeclared function: " + var->name);
                    call->type = &TypeVoid;
                    isFunctionCall = true;
                }
                if (isFunctionCall) {
                    if (targetFn && (targetFn->name == "free" || hasAttribute(targetFn->attributes, AttrKind::Free)) && call->args.size() == 1) {
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
            if (!calleeType->isVariadicFunc && call->args.size() != calleeType->paramTypes.size()) {
                error(call->loc, "Function pointer expects " + std::to_string(calleeType->paramTypes.size()) + " args.");
            }
            if (calleeType->isVariadicFunc && call->args.size() < calleeType->paramTypes.size() - 1) {
                error(call->loc, "Function pointer expects at least " + std::to_string(calleeType->paramTypes.size() - 1) + " args.");
            }
            for (size_t i = 0; i < call->args.size(); ++i) {
                if (calleeType && i < calleeType->paramTypes.size()) {
                    Type* expectedParam = calleeType->paramTypes[i];
                    if (expectedParam && expectedParam->name.starts_with("heap ")) {
                        if (auto argVar = dynamic_cast<VarExpr*>(call->args[i].get())) {
                            for (Scope* s = m_currentScope; s; s = s->parent) {
                                if (s->variables.count(argVar->name)) {
                                    s->movedVars.insert(argVar->name);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            call->type = calleeType->returnType;
        }
        else if (auto cast = dynamic_cast<CastExpr*>(&expr)) {
            checkExpr(*cast->operand);

            if (!cast->operand->type)
                cast->operand->type = &TypeVoid;

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
        else if (auto builtin = dynamic_cast<BuiltinCallExpr*>(&expr)) {
            for (auto& arg : builtin->args) checkExpr(*arg);

            switch (builtin->builtinType) {
                case TokenType::BuiltinMemfill:
                    if (builtin->args.size() != 3) error(builtin->loc, "__builtin_memfill requires 3 arguments");
                    builtin->type = &TypeVoid;
                    break;
                case TokenType::BuiltinMemcpy:
                    if (builtin->args.size() != 3) error(builtin->loc, "__builtin_memcpy requires 3 arguments");
                    builtin->type = &TypeVoid;
                    break;
                case TokenType::BuiltinTrap:
                case TokenType::BuiltinUnreachable:
                    if (!builtin->args.empty()) error(builtin->loc, "Builtin takes no arguments");
                    builtin->type = &TypeVoid;
                    break;
                case TokenType::BuiltinBswap:
                    if (builtin->args.size() != 1) error(builtin->loc, "bswap requires 1 argument");
                    builtin->type = builtin->args.empty() ? &TypeVoid : builtin->args[0]->type;
                    break;
                case TokenType::BuiltinAllocate:
                    if (builtin->args.size() != 2) error(builtin->loc, "alloc requires 2 arguments");
                    {
                        ParsedType pt;
                        pt.baseName = "u8";
                        pt.modifiers.push_back(TypeModifier::Owner);
                        builtin->type = resolveType(pt);
                    }
                    break;
                default:
                    builtin->type = &TypeVoid;
                    break;
            }
        }
        else if (auto hasMeth = dynamic_cast<CompilerHasMethodExpr*>(&expr)) {
            Type* targetT = resolveType(hasMeth->parsedTargetType);
            bool found = false;
            if (targetT) {
                std::string typeName = targetT->isPointer() ? targetT->base->name : targetT->name;
                std::string expectedMethod = typeName + "::" + hasMeth->methodName;
                if (m_functions.count(expectedMethod) || m_generic_functions.count(expectedMethod)) {
                    found = true;
                }
            }
            hasMeth->resultValue = found;
            hasMeth->type = &TypeBool;
        }
        else if (auto al = dynamic_cast<AlignofExpr*>(&expr)) {
            al->resolvedTargetType = resolveType(al->parsedTargetType);
            if (!al->resolvedTargetType) error(al->loc, "Unknown type in alignof");
            al->type = &TypeU64;
        }
        else if (auto exp = dynamic_cast<ExpandExpr*>(&expr)) {
            checkExpr(*exp->operand);
            exp->type = exp->operand->type;
        }
        else if (auto le = dynamic_cast<LockExpr*>(&expr)) {
            checkExpr(*le->operand);
            expr.type = le->operand->type;
        }
    }
    
    void Sema::enterScope() {
        Scope* s = new Scope();
        s->parent = m_currentScope;
        m_currentScope = s;
        t_attrScopes.push_back({});
    }
    void Sema::exitScope() {
        Scope* old = m_currentScope;
        m_currentScope = old->parent;
        delete old;
        t_attrScopes.pop_back();
    }
    bool Sema::declareVariable(const std::string& name, Type* type, SourceLoc loc, const std::vector<Attribute>& attrs) {
        if (m_currentScope->variables.count(name)) {
            error(loc, "Redeclaration of variable '" + name + "' in the current scope.");
            return false;
        }
        m_currentScope->variables[name] = type;
        if (!t_attrScopes.empty()) t_attrScopes.back()[name] = attrs;
        if (type->name.starts_with("heap ") && !hasAttribute(attrs, AttrKind::Nofree)) {
            m_currentScope->ownedVars.insert(name);
        }
        return true;
    }
    bool Sema::declareVariable(const std::string& name, Type* type, SourceLoc loc) {
        if (m_currentScope->variables.count(name)) {
            error(loc, "Redeclaration of variable '" + name + "' in the current scope.");
            return false;
        }

        m_currentScope->variables[name] = type;
        return true;
    }
    Type* Sema::lookupVariable(const std::string& name) {
        for (Scope* s = m_currentScope; s; s = s->parent)
            if (s->variables.count(name)) return s->variables[name];
        return nullptr;
    }
}