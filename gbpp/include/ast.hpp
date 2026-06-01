#pragma once
#include "token.hpp"
#include "type.hpp"
#include <vector>
#include <memory>
#include <string>
#include <set>

namespace gbpp {

    enum class TypeModifier { None, Owner, Ref, Volatile, Const };

    struct ParsedType {
        std::string baseName;
        std::vector<TypeModifier> modifiers;
        bool isArray = false;
        std::string arraySizeExpr = "";

        bool isFunction = false;
        std::vector<ParsedType> paramTypes;
        std::shared_ptr<ParsedType> returnType;
        std::vector<ParsedType> genericArgs;

        bool hasModifier(TypeModifier mod) const {
            for (auto m : modifiers) {
                if (m == mod) return true;
            }
            return false;
        };

        std::string toString() const {
            if (isFunction) {
                std::string res = "fn(";
                for (size_t i = 0; i < paramTypes.size(); ++i) {
                    res += paramTypes[i].toString();
                    if (i < paramTypes.size() - 1) res += ",";
                }
                res += "):";
                res += returnType ? returnType->toString() : "void";
                return res;
            }

            std::string res = "";

            for (auto mod : modifiers) {
                if (mod == TypeModifier::Owner) res += "owner ";
                else if (mod == TypeModifier::Ref) res += "ref ";
                else if (mod == TypeModifier::Volatile) res += "volatile ";
                else if (mod == TypeModifier::Const) res += "const ";
            }

            res += baseName;

            if (!genericArgs.empty()) {
                res += "<";
                for (size_t i = 0; i < genericArgs.size(); ++i) {
                    res += genericArgs[i].toString();
                    if (i < genericArgs.size() - 1) res += ", ";
                }
                res += ">";
            }

            if (isArray) {
                res += "[";
                res += arraySizeExpr;
                res += "]";
            }
            return res;
        }
    };

    struct Expr;
    struct Stmt;

    struct ASTNode {
        SourceLoc loc;
        virtual ~ASTNode() = default;
    };

    struct Expr : ASTNode {
        Type* type = nullptr;
    };

    struct IntLiteral : Expr { std::string value; };
    struct FloatLiteral : Expr { std::string value; };
    struct StringLiteral : Expr { std::string value; };
    struct NullLiteral : Expr {};
    struct VarExpr : Expr { std::string name; };

    struct DerefExpr : Expr {
        std::unique_ptr<Expr> operand;
    };

    struct ArrayAccessExpr : Expr {
        std::unique_ptr<Expr> array;
        std::unique_ptr<Expr> index;
    };

    struct CallExpr : Expr {
        std::unique_ptr<Expr> callee;
        std::vector<std::unique_ptr<Expr>> args;
    };

    struct StructInitExpr : Expr {
        std::string structName;
        struct FieldInit {
            std::string name;
            std::unique_ptr<Expr> value;
        };
        std::vector<FieldInit> fields;
    };

    struct SizeofExpr : Expr {
        ParsedType parsedTargetType;
        Type* resolvedTargetType = nullptr;
    };

    enum class CastKind { Value, Bits };
    struct CastExpr : Expr {
        CastKind castKind;
        Type* targetType = nullptr;
        ParsedType parsedTargetType;
        std::unique_ptr<Expr> operand;
    };

    struct ImportDecl : ASTNode {
        bool isLib;
        std::string path;
    };

    struct MemberExpr : Expr {
        std::unique_ptr<Expr> object;
        std::string memberName;
    };

    struct AllocExpr : Expr {
        ParsedType parsedTargetType;
        Type* resolvedTargetType = nullptr;
        std::vector<std::unique_ptr<Expr>> args;
        std::string initMethodName;
    };

    struct AssignmentExpr : Expr {
        std::unique_ptr<Expr> target;
        TokenType op = TokenType::Equal;
        std::unique_ptr<Expr> value;
    };

    struct BinaryExpr : Expr {
        std::unique_ptr<Expr> left;
        TokenType op;
        std::unique_ptr<Expr> right;
    };

    struct Stmt : ASTNode {};

    struct VarDecl : Stmt {
        std::string name;
        ParsedType parsedType;
        Type* resolvedType = nullptr;
        std::unique_ptr<Expr> initializer;
        std::set<std::string> attributes;
    };

    struct AddrOfExpr : Expr {
        std::unique_ptr<Expr> operand;
    };

    struct AsmStmt : Stmt {
        std::string assembly;
    };

    struct ReturnStmt : Stmt { std::unique_ptr<Expr> value; };
    struct ExprStmt : Stmt { std::unique_ptr<Expr> expr; };
    struct BlockStmt : Stmt { std::vector<std::unique_ptr<Stmt>> statements; };

    struct IfStmt : Stmt {
        std::unique_ptr<Expr> condition;
        std::unique_ptr<Stmt> thenBranch;
        std::unique_ptr<Stmt> elseBranch;
    };
    struct WhileStmt : Stmt {
        std::unique_ptr<Expr> condition;
        std::unique_ptr<Stmt> body;
    };
    struct ForStmt : public Stmt {
        std::unique_ptr<Stmt> init;
        std::unique_ptr<Expr> condition;
        std::unique_ptr<Expr> update;
        std::unique_ptr<Stmt> body;
    };

    struct GenericParam { std::string name; };

    struct FunctionDecl : ASTNode {
        std::vector<GenericParam> genericParams;
        std::set<std::string> attributes;
        std::string name;
        struct Param {
            std::string name;
            ParsedType parsedType;
            Type* resolvedType = nullptr;
        };
        std::vector<Param> params;
        ParsedType returnType;
        Type* returnTypeResolved = nullptr;
        std::unique_ptr<Type> signatureType = nullptr;
        std::unique_ptr<BlockStmt> body;
    };

    struct UnaryExpr : Expr {
        TokenType op;
        std::unique_ptr<Expr> operand;
    };

    struct StructDecl : ASTNode {
        std::vector<GenericParam> genericParams;
        std::set<std::string> attributes;
        std::string name;
        struct Field { std::string name; ParsedType parsedType; int offset; };
        std::vector<Field> fields;
        std::vector<std::unique_ptr<FunctionDecl>> methods;
    };

    struct EnumDecl : ASTNode {
        std::set<std::string> attributes;
        std::string name;
        struct EnumMember { std::string name; uint64_t value; };
        std::vector<EnumMember> members;
    };

    struct EnumAccessExpr : Expr {
        std::string enumName;
        std::string memberName;
        uint64_t value = 0;
    };

    struct AliasDecl : ASTNode {
        std::set<std::string> attributes;
        std::string name;
        ParsedType targetType;
    };

    struct BreakStmt : Stmt {};

    struct SelfFieldExpr : Expr {
        std::string fieldName;
    };

    struct Program : ASTNode {
        std::vector<std::unique_ptr<ImportDecl>> imports;
        std::vector<std::unique_ptr<AliasDecl>> aliases;
        std::vector<std::unique_ptr<EnumDecl>> enums;
        std::vector<std::unique_ptr<StructDecl>> structs;
        std::vector<std::unique_ptr<FunctionDecl>> functions;
        std::vector<std::unique_ptr<VarDecl>> globalVars;
    };
}