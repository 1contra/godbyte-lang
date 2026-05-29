#pragma once
#include "ast.hpp"
#include "ir.hpp"
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>

namespace gbpp {

    class IRGenerator {
    public:
        IRModule generate(const Program& prog);
        void optimize(IRModule& mod, bool enableOpt);
        std::unordered_map<std::string, int> m_stackSlots;

    private:
        IRModule m_module;
        IRFunction* m_currentFunc = nullptr;

        std::map<std::string, int> m_locals;

        std::unordered_map<std::string, StructDecl*> m_structMap;
        BasicBlock* m_exitBlock = nullptr;
        BasicBlock* m_currentBlock = nullptr;
        std::vector<BasicBlock*> m_loopExits;
        std::unordered_set<std::string> m_stackPrimitives;

        int m_exitLabel = -1;
        int m_retReg = -1;

        void genFunction(const FunctionDecl& fn);
        void genBlock(const BlockStmt& block);
        void genStmt(const Stmt& stmt);
        int genExpr(const Expr& expr);
        void emit(Instruction inst);
        int getOffset(Type* type, const std::string& field);
    };

}