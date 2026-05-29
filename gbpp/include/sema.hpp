#pragma once
#include "ast.hpp"
#include <unordered_map>
#include <map>
#include <vector>
#include <set>

namespace gbpp {

    struct Scope {
        std::unordered_map<std::string, Type*> variables;
        std::set<std::string> ownedVars;
        std::set<std::string> freedVars;
        Scope* parent = nullptr;
    };

    class Sema {
    public:
        Sema();
        bool analyze(Program& prog);
        bool analyzeModules(const std::vector<Program*>& progs);

        std::unordered_map<std::string, StructDecl*> m_structs;
        std::unordered_map<std::string, FunctionDecl*> m_functions;
        std::unordered_map<std::string, EnumDecl*> m_enums;
        std::unordered_map<std::string, ConstDecl*> m_constants;
        std::map<std::string, ParsedType> m_aliases;

        std::vector<std::string> errors;

        Type* resolveType(const ParsedType& pt);

        std::unordered_map<std::string, StructDecl*> m_generic_structs;
        std::vector<std::unique_ptr<StructDecl>> m_instantiated_structs;

        std::unordered_map<std::string, FunctionDecl*> m_generic_functions;
        std::vector<std::unique_ptr<FunctionDecl>> m_instantiated_functions;
        std::vector<FunctionDecl*> m_pending_function_checks;

        StructDecl* instantiateStruct(StructDecl* tmpl, const std::vector<ParsedType>& args, const std::string& mangledName);
        FunctionDecl* instantiateFunction(FunctionDecl* tmpl, const std::vector<ParsedType>& args, const std::string& mangledName);
        ParsedType substituteType(ParsedType pt, const std::unordered_map<std::string, std::string>& subs);

        std::unique_ptr<Expr> cloneExpr(Expr* e, const std::unordered_map<std::string, std::string>& subs);
        std::unique_ptr<Stmt> cloneStmt(Stmt* s, const std::unordered_map<std::string, std::string>& subs);

    private:
        Scope* m_currentScope = nullptr;
        Type* m_currentFunctionReturnType = nullptr;

        void error(SourceLoc loc, const std::string& msg);

        void enterScope();
        void exitScope();
        bool declareVariable(const std::string& name, Type* type, SourceLoc loc, const std::set<std::string>& attrs);
        bool declareVariable(const std::string& name, Type* type, SourceLoc loc);
        Type* lookupVariable(const std::string& name);

        void checkFunction(FunctionDecl& fn);
        void checkBlock(BlockStmt& block);
        void checkStmt(Stmt& stmt);

        void checkExpr(Expr& expr);
    };

}