/**
 * Copyright 2026 1contra
 *
 * Licensed under the GNU General Public License, Version 3
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.gnu.org/licenses/gpl-3.0.html
 */

#pragma once
#include "server.hpp"
#include "../include/lexer.hpp"
#include "../include/parser.hpp"
#include "../include/sema.hpp"
#include <format>
#include <sstream>
#include <vector>
#include <string>
#include <memory>
#include <iostream>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <chrono>

namespace gbpp::lsp {

    static std::vector<std::string> getLspNamespaceCandidates(const std::string& currentNamespace, const std::string& name) {
        std::vector<std::string> candidates;
        if (name.empty()) return candidates;
        if (name.starts_with("::")) {
            candidates.push_back(name.substr(2));
            return candidates;
        }
        std::string ns = currentNamespace;
        while (!ns.empty()) {
            candidates.push_back(ns + "::" + name);
            size_t pos = ns.rfind("::");
            if (pos != std::string::npos) {
                ns = ns.substr(0, pos);
            }
            else {
                ns = "";
            }
        }
        candidates.push_back(name);
        return candidates;
    }

    template <typename MapType>
    static std::string findInLspMap(const MapType& map, const std::string& name, const std::string& curNs) {
        for (const auto& cand : getLspNamespaceCandidates(curNs, name)) {
            if (map.count(cand)) return cand;
        }
        return "";
    }

    std::unordered_map<std::string, std::unique_ptr<Program>> documentPrograms;
    std::unordered_map<std::string, std::unique_ptr<Sema>> documentSemas;

    void loadImportsRecursively(Program* mainProgram, const std::string& currentUri, std::unordered_map<std::string, std::vector<Token>>& tokenCache) {
        std::string basePath = currentUri;
        if (basePath.starts_with("file://")) basePath = basePath.substr(7);
#ifdef _WIN32
        if (basePath.starts_with("/")) basePath = basePath.substr(1);
#endif
        std::filesystem::path currentDir = std::filesystem::path(basePath).parent_path();

        for (const auto& imp : mainProgram->imports) {
            std::filesystem::path targetPath;

            if (imp->isLib) {
                targetPath = std::filesystem::current_path() / "libs" / (imp->path + ".gbpp");

                if (!std::filesystem::exists(targetPath)) {
                    const char* envHome = std::getenv("DIVO_HOME");
                    if (envHome) {
                        targetPath = std::filesystem::path(envHome) / "libs" / (imp->path + ".gbpp");
                    }
                }

                if (!std::filesystem::exists(targetPath)) {
                    targetPath = std::filesystem::current_path() / "std" / (imp->path + ".gbpp");
                }

                if (!std::filesystem::exists(targetPath)) {
                    targetPath = currentDir / (imp->path + ".gbpp");
                }
            }
            else {
                targetPath = currentDir / imp->path;
            }

            if (std::filesystem::exists(targetPath)) {
                std::string pathStr = targetPath.string();
                std::vector<Token> tokens;

                if (tokenCache.count(pathStr)) {
                    tokens = tokenCache[pathStr];
                }
                else {
                    std::ifstream file(targetPath);
                    std::string content = std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                    Lexer lexer(content, pathStr);
                    tokens = lexer.tokenize();
                    tokenCache[pathStr] = tokens;
                }

                Parser parser(tokens);

                if (auto importedProg = parser.parse()) {
                    loadImportsRecursively(importedProg.get(), "file://" + pathStr, tokenCache);

                    for (auto& st : importedProg->structs) {
                        mainProgram->structs.push_back(std::move(st));
                    }
                    for (auto& fn : importedProg->functions) {
                        mainProgram->functions.push_back(std::move(fn));
                    }
                    for (auto& alias : importedProg->aliases) {
                        mainProgram->aliases.push_back(std::move(alias));
                    }
                    for (auto& enm : importedProg->enums) {
                        mainProgram->enums.push_back(std::move(enm));
                    }
                    for (auto& var : importedProg->globalVars) {
                        mainProgram->globalVars.push_back(std::move(var));
                    }
                }
            }
            else {
                throw std::runtime_error("Could not find import: " + targetPath.string());
            }
        }
    }

    void LSPServer::run() {
        isRunning = true;
        workerThread = std::thread(&LSPServer::workerLoop, this);

        while (std::cin) {
            std::string line;
            int contentLength = 0;

            while (std::getline(std::cin, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }

                if (line.empty()) {
                    break;
                }

                if (line.starts_with("Content-Length: ")) {
                    try {
                        contentLength = std::stoi(line.substr(16));
                    }
                    catch (...) {}
                }
            }

            if (contentLength > 0) {
                std::vector<char> buffer(contentLength);
                std::cin.read(buffer.data(), contentLength);

                try {
                    json request = json::parse(std::string(buffer.begin(), buffer.end()));
                    {
                        std::lock_guard<std::mutex> lock(queueMutex);
                        messageQueue.push(request);
                    }
                    queueCV.notify_one();
                }
                catch (...) {}
            }
        }
        isRunning = false;
        queueCV.notify_all();
        if (workerThread.joinable()) workerThread.join();
    }

    LSPServer::~LSPServer() {
        isRunning = false;
        queueCV.notify_all();
        if (workerThread.joinable()) {
            workerThread.join();
        }
    }

    void LSPServer::workerLoop() {
        while (isRunning) {
            std::unique_lock<std::mutex> lock(queueMutex);

            if (messageQueue.empty()) {
                if (needsCompilation) {
                    if (queueCV.wait_until(lock, compileDeadline) == std::cv_status::timeout) {
                        needsCompilation = false;
                        std::string uriToCompile = dirtyUri;
                        std::string textToCompile = documents[dirtyUri];
                        lock.unlock();
                        compileAndPublishDiagnostics(uriToCompile, textToCompile);
                    }
                }
                else {
                    queueCV.wait(lock, [this] { return !messageQueue.empty() || !isRunning; });
                }
            }
            else {
                auto msg = messageQueue.front();
                messageQueue.pop();

                std::string method = msg.contains("method") ? msg["method"].get<std::string>() : "";

                if (needsCompilation && (
                    method == "textDocument/semanticTokens/full" ||
                    method == "textDocument/hover" ||
                    method == "textDocument/definition" ||
                    method == "textDocument/completion")) {

                    needsCompilation = false;
                    std::string uriToCompile = dirtyUri;
                    std::string textToCompile = documents[dirtyUri];
                    lock.unlock();

                    compileAndPublishDiagnostics(uriToCompile, textToCompile);
                    handleMessage(msg);
                }
                else {
                    lock.unlock();
                    handleMessage(msg);
                }
            }
        }
    }

    std::string getDocComment(const SourceLoc& loc, const std::unordered_map<std::string, std::string>& docs) {
        if (loc.line <= 1 || loc.filename.empty()) return "";

        std::string source;
        std::string searchUri = loc.filename;

        if (!searchUri.starts_with("file://")) {
            std::string fixed = searchUri;
            std::replace(fixed.begin(), fixed.end(), '\\', '/');
            if (fixed.length() >= 2 && fixed[1] == ':') fixed = "/" + fixed;
            searchUri = "file://" + fixed;
        }

        if (docs.count(searchUri)) source = docs.at(searchUri);
        else if (docs.count(loc.filename)) source = docs.at(loc.filename);
        else {
            std::ifstream file(loc.filename);
            if (file) {
                source = std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            }
            else return "";
        }

        std::vector<std::string> lines;
        std::stringstream ss(source);
        std::string s;
        while (std::getline(ss, s)) lines.push_back(s);

        if (loc.line < 2 || loc.line > lines.size() + 1) return "";

        int idx = loc.line - 2;
        std::vector<std::string> extracted;
        bool inBlock = false;

        while (idx >= 0) {
            std::string l = lines[idx];
            if (!l.empty() && l.back() == '\r') l.pop_back();

            size_t firstNonSpace = l.find_first_not_of(" \t");
            if (firstNonSpace == std::string::npos) {
                if (inBlock) extracted.push_back("");
                else break;
            }
            else {
                l = l.substr(firstNonSpace);
                if (inBlock) {
                    if (l.starts_with("/**") || l.starts_with("/*")) {
                        inBlock = false;
                        size_t prefixLen = l.starts_with("/**") ? 3 : 2;
                        if (l.length() > prefixLen) {
                            std::string rest = l.substr(prefixLen);
                            size_t startText = rest.find_first_not_of(" \t");
                            if (startText != std::string::npos && rest.find("*/") == std::string::npos) {
                                extracted.push_back(rest.substr(startText));
                            }
                        }
                    }
                    else if (l.starts_with("* ")) extracted.push_back(l.substr(2));
                    else if (l.starts_with("*")) extracted.push_back(l.substr(1));
                    else extracted.push_back(l);
                }
                else {
                    if (l.ends_with("*/")) {
                        inBlock = true;
                        if (l.starts_with("/**")) {
                            inBlock = false;
                            size_t start = l.find("/**") + 3;
                            size_t end = l.rfind("*/");
                            if (end > start) {
                                std::string inner = l.substr(start, end - start);
                                size_t fns = inner.find_first_not_of(" \t");
                                if (fns != std::string::npos) extracted.push_back(inner.substr(fns));
                            }
                        }
                    }
                    else if (l.starts_with("///")) {
                        size_t start = 3;
                        if (l.length() > 3 && l[3] == ' ') start = 4;
                        extracted.push_back(l.substr(start));
                    }
                    else {
                        break;
                    }
                }
            }
            idx--;
        }

        if (extracted.empty()) return "";

        std::reverse(extracted.begin(), extracted.end());
        std::string res = "\n\n---\n\n";
        for (const auto& line : extracted) {
            res += line + "\n";
        }
        return res;
    }

    void LSPServer::handleHover(const json& msg) {
        int reqLine = msg["params"]["position"]["line"];
        int reqChar = msg["params"]["position"]["character"];
        std::string uri = msg["params"]["textDocument"]["uri"];

        auto* currentSema = documentSemas.count(uri) ? documentSemas[uri].get() : nullptr;
        auto* currentProgram = documentPrograms.count(uri) ? documentPrograms[uri].get() : nullptr;

        std::string text = documents[uri];
        Lexer lexer(text, uri);
        auto tokens = lexer.tokenize();

        std::string hoverMarkdown;

        auto makeSection = [](const std::string& title, const std::string& body) {
            return "\n\n---\n\n### " + title + "\n" + body;
            };

        auto makeCodeBlock = [](const std::string& code) {
            return "```gbpp\n" + code + "\n```";
        };

        auto makeBullet = [](const std::string& k, const std::string& v) {
            return "- **" + k + "**: " + v + "\n";
        };

        std::string rawUri = uri;
        if (rawUri.starts_with("file://")) rawUri = rawUri.substr(7);

#ifdef _WIN32
        if (rawUri.starts_with("/")) rawUri = rawUri.substr(1);
#endif

        std::vector<std::pair<std::string, int>> nsStack;
        std::string currentNamespace;
        int braceDepth = 0;

        std::string activeStruct = "";
        int structBraceDepth = -1;

        for (size_t i = 0; i < tokens.size(); ++i) {
            const auto& token = tokens[i];

            if (token.type == TokenType::Struct && i + 1 < tokens.size() && tokens[i + 1].type == TokenType::Identifier) {
                activeStruct = tokens[i + 1].text;
            }

            if (token.type == TokenType::LBrace) {
                braceDepth++;
                if (!activeStruct.empty() && structBraceDepth == -1) {
                    structBraceDepth = braceDepth;
                }
            }
            else if (token.type == TokenType::RBrace) {
                if (structBraceDepth == braceDepth) {
                    activeStruct = "";
                    structBraceDepth = -1;
                }
                braceDepth--;

                if (!nsStack.empty() && nsStack.back().second == braceDepth) {
                    nsStack.pop_back();

                    currentNamespace.clear();

                    for (const auto& ns : nsStack) {
                        if (!currentNamespace.empty()) currentNamespace += "::";
                        currentNamespace += ns.first;
                    }
                }
            }
            else if (token.type == TokenType::Namespace && i + 1 < tokens.size() && tokens[i + 1].type == TokenType::Identifier) {
                std::string nsName = tokens[i + 1].text;
                nsStack.push_back({ nsName, braceDepth });

                if (currentNamespace.empty())
                    currentNamespace = nsName;
                else
                    currentNamespace += "::" + nsName;
            }

            bool hovered =
                (token.loc.line - 1 == reqLine) &&
                (reqChar >= (int)token.loc.col - 1) &&
                (reqChar < (int)(token.loc.col - 1 + token.text.length()));

            if (!hovered) continue;

            auto cleanTypeName = [&](std::string tName) {
                if (tName.starts_with("ref ")) tName = tName.substr(4);
                if (tName.starts_with("owner ")) tName = tName.substr(6);

                size_t bracket = tName.find('[');
                if (bracket != std::string::npos)
                    tName = tName.substr(0, bracket);

                size_t angleBracket = tName.find('<');
                if (angleBracket != std::string::npos)
                    tName = tName.substr(0, angleBracket);

                while (currentSema && currentSema->m_aliases.count(tName)) {
                    tName = currentSema->m_aliases[tName].baseName;
                }

                return tName;
                };

            auto getVarType = [&](const std::string& searchVarName) {
                std::string typeFound = "";

                if (currentProgram) {
                    std::function<std::string(const BlockStmt*)> searchBlock = [&](const BlockStmt* block) -> std::string {
                        if (!block) return "";
                        for (const auto& stmt : block->statements) {
                            if (auto decl = dynamic_cast<VarDecl*>(stmt.get())) {
                                if (decl->name == searchVarName) return decl->parsedType.toString();
                            }
                            else if (auto b = dynamic_cast<BlockStmt*>(stmt.get())) {
                                auto res = searchBlock(b);
                                if (!res.empty()) return res;
                            }
                            else if (auto i_stmt = dynamic_cast<IfStmt*>(stmt.get())) {
                                auto res = searchBlock(dynamic_cast<BlockStmt*>(i_stmt->thenBranch.get()));
                                if (!res.empty()) return res;
                                if (i_stmt->elseBranch) {
                                    res = searchBlock(dynamic_cast<BlockStmt*>(i_stmt->elseBranch.get()));
                                    if (!res.empty()) return res;
                                }
                            }
                            else if (auto w = dynamic_cast<WhileStmt*>(stmt.get())) {
                                auto res = searchBlock(dynamic_cast<BlockStmt*>(w->body.get()));
                                if (!res.empty()) return res;
                            }
                            else if (auto f = dynamic_cast<ForStmt*>(stmt.get())) {
                                if (f->init) {
                                    if (auto decl = dynamic_cast<VarDecl*>(f->init.get())) {
                                        if (decl->name == searchVarName) return decl->parsedType.toString();
                                    }
                                }
                                if (f->body) {
                                    auto res = searchBlock(dynamic_cast<BlockStmt*>(f->body.get()));
                                    if (!res.empty()) return res;
                                }
                            }
                        }
                        return "";
                        };

                    for (const auto& fn : currentProgram->functions) {
                        for (const auto& p : fn->params) {
                            if (p.name == searchVarName) typeFound = p.parsedType.toString();
                        }
                        if (fn->body && typeFound.empty()) {
                            typeFound = searchBlock(fn->body.get());
                        }
                    }
                }

                if (typeFound.empty()) {
                    size_t absPos = 0;
                    int lineCnt = 0;
                    for (size_t k = 0; k < text.length(); ++k) {
                        if (lineCnt == reqLine) { absPos = k + reqChar; break; }
                        if (text[k] == '\n') lineCnt++;
                    }

                    std::string searchPattern = searchVarName + ":";
                    size_t pos = text.rfind(searchPattern, absPos);
                    if (pos != std::string::npos) {
                        size_t typeStart = pos + searchPattern.length();
                        while (typeStart < text.length() && (text[typeStart] == ' ' || text[typeStart] == '\t')) typeStart++;

                        int angleDepth = 0;
                        for (size_t k = typeStart; k < text.length(); ++k) {
                            char c = text[k];
                            if (c == '<') angleDepth++;
                            else if (c == '>') angleDepth--;
                            else if (angleDepth == 0 && (c == ' ' || c == ';' || c == '=' || c == '\n' || c == ',' || c == ')')) break;
                            typeFound += c;
                        }
                    }
                }

                return typeFound;
                };

            auto appendDocs = [&](const SourceLoc& loc) {
                std::string docs = getDocComment(loc, documents);

                if (!docs.empty()) {
                    hoverMarkdown += makeSection("Documentation", docs);
                }
            };

            auto resolveDotChain = [&](int tokenIdx) -> std::pair<StructDecl*, StructDecl::Field*> {
                if (!currentSema)
                    return { nullptr, nullptr };

                std::vector<std::string> chain;
                int idx = tokenIdx;

                while (idx >= 0 && tokens[idx].type == TokenType::Identifier) {
                    chain.push_back(tokens[idx].text);
                    if (idx >= 2 && tokens[idx - 1].type == TokenType::Dot)
                        idx -= 2;
                    else
                        break;
                }

                std::reverse(chain.begin(), chain.end());

                if (chain.size() < 2)
                    return { nullptr, nullptr };

                std::string currentTypeName = cleanTypeName(getVarType(chain[0]));
                if (currentTypeName.empty())
                    return { nullptr, nullptr };

                std::string resolvedType = findInLspMap(currentSema->m_structs, currentTypeName, currentNamespace);
                if (resolvedType.empty()) resolvedType = findInLspMap(currentSema->m_generic_structs, currentTypeName, currentNamespace);
                if (!resolvedType.empty()) currentTypeName = resolvedType;

                StructDecl* currentStruct = nullptr;
                StructDecl::Field* targetField = nullptr;

                for (size_t k = 1; k < chain.size(); ++k) {

                    if (currentSema->m_structs.count(currentTypeName)) {
                        currentStruct = currentSema->m_structs[currentTypeName];
                    }
                    else if (currentSema->m_generic_structs.count(currentTypeName)) {
                        currentStruct = currentSema->m_generic_structs[currentTypeName];
                    }
                    else {
                        return { nullptr, nullptr };
                    }

                    bool foundField = false;

                    for (auto& field : currentStruct->fields) {
                        if (field.name == chain[k]) {
                            targetField = &field;
                            currentTypeName = cleanTypeName(field.parsedType.toString());
                            foundField = true;
                            break;
                        }
                    }

                    if (!foundField) return { nullptr, nullptr };
                }
                return { currentStruct, targetField };
            };

            if (token.type == TokenType::Identifier && currentSema) {
                bool resolved = false;

                int l = i;
                while (l >= 2 && tokens[l - 1].type == TokenType::DoubleColon) l -= 2;

                std::string fqn = "";
                for (int k = l; k <= i; ++k) fqn += tokens[k].text;

                std::string scopedFqn = currentNamespace.empty() ? fqn : currentNamespace + "::" + fqn;
                std::string methodFullName;

                if (i >= 2 && tokens[i - 1].type == TokenType::DoubleColon) {
                    int baseIdx = i - 2;
                    if (baseIdx >= 0 && tokens[baseIdx].type == TokenType::GT) {
                        int depth = 1;
                        baseIdx--;
                        while (baseIdx >= 0 && depth > 0) {
                            if (tokens[baseIdx].type == TokenType::GT) depth++;
                            else if (tokens[baseIdx].type == TokenType::LT) depth--;
                            baseIdx--;
                        }
                    }
                    std::string parentFqn = (baseIdx >= 0 && tokens[baseIdx].type == TokenType::Identifier) ? tokens[baseIdx].text : "";
                    if (!parentFqn.empty() && (currentSema->m_structs.count(parentFqn) || currentSema->m_generic_structs.count(parentFqn))) {
                        if (currentSema->m_functions.count(parentFqn + "::" + token.text) || currentSema->m_generic_functions.count(parentFqn + "::" + token.text)) {
                            methodFullName = parentFqn + "::" + token.text;
                        }
                        else {
                            methodFullName = parentFqn + "_" + token.text;
                        }
                    }
                }
                else if (i >= 2 && tokens[i - 1].type == TokenType::Dot) {

                    auto [parentStruct, targetField] = resolveDotChain(i);

                    if (parentStruct && targetField) {
                        Type* ft = currentSema->resolveType(targetField->parsedType);
                        int fSize = ft ? ft->sizeBytes : 8;

                        hoverMarkdown += makeCodeBlock(targetField->name + ": " + targetField->parsedType.toString());

                        std::string meta;
                        meta += makeBullet("Kind", "Struct Field");
                        meta += makeBullet("Parent Type", "`" + parentStruct->name + "`");

                        if (parentStruct->genericParams.empty()) {
                            meta += makeBullet("Offset", "`0x" + std::format("{:X}", targetField->offset) + "`");
                            meta += makeBullet("Size", "`" + std::to_string(fSize) + " bytes`");
                        }
                        else {
                            meta += makeBullet("Offset", "Dependent on generic arguments");
                            meta += makeBullet("Size", "Dependent on generic arguments");
                        }

                        if (targetField->parsedType.hasModifier(TypeModifier::Owner))
                            meta += makeBullet("Memory", "Owning heap pointer");
                        else if (targetField->parsedType.hasModifier(TypeModifier::Ref))
                            meta += makeBullet("Memory", "Borrowed reference");

                        hoverMarkdown += makeSection("Field Info", meta);
                        appendDocs(parentStruct->loc);
                        resolved = true;
                    }
                    else {
                        auto [pStruct, pField] = resolveDotChain(i - 2);
                        std::string vType = pField ? cleanTypeName(pField->parsedType.toString()) : cleanTypeName(getVarType(tokens[i - 2].text));
                        if (!vType.empty()) {
                            if (currentSema->m_functions.count(vType + "::" + token.text) || currentSema->m_generic_functions.count(vType + "::" + token.text)) {
                                methodFullName = vType + "::" + token.text;
                            }
                            else {
                                methodFullName = vType + "_" + token.text;
                            }
                        }
                    }
                }

                if (!resolved && !methodFullName.empty() && (currentSema->m_functions.count(methodFullName) || currentSema->m_generic_functions.count(methodFullName))) {
                    auto fn = currentSema->m_functions.count(methodFullName) ? currentSema->m_functions[methodFullName] : currentSema->m_generic_functions[methodFullName];
                    std::string sig = "fn " + fn->name + "(";
                    for (size_t p = 0; p < fn->params.size(); ++p) {
                        sig += fn->params[p].name + ": " + fn->params[p].parsedType.toString();
                        if (p + 1 < fn->params.size()) sig += ", ";
                    }
                    sig += "): " + fn->returnType.toString();
                    hoverMarkdown += makeCodeBlock(sig);
                    std::string meta;
                    meta += makeBullet("Kind", "Method");
                    meta += makeBullet("Return Type", "`" + fn->returnType.toString() + "`");
                    if (!currentNamespace.empty()) meta += makeBullet("Namespace", "`" + currentNamespace + "`");
                    hoverMarkdown += makeSection("Symbol Info", meta);
                    appendDocs(fn->loc);
                    resolved = true;
                }
                else if (!resolved) {
                    std::string resFn = findInLspMap(currentSema->m_functions, fqn, currentNamespace);
                    std::string resGenFn = findInLspMap(currentSema->m_generic_functions, fqn, currentNamespace);
                    std::string resSt = findInLspMap(currentSema->m_structs, fqn, currentNamespace);
                    std::string resGenSt = findInLspMap(currentSema->m_generic_structs, fqn, currentNamespace);
                    std::string resEnm = findInLspMap(currentSema->m_enums, fqn, currentNamespace);
                    std::string resAlias = findInLspMap(currentSema->m_aliases, fqn, currentNamespace);

                    if (!resFn.empty() || !resGenFn.empty()) {
                        FunctionDecl* fn = !resFn.empty() ? currentSema->m_functions[resFn] : currentSema->m_generic_functions[resGenFn];
                        std::string sig = "fn " + fn->name;
                        if (!fn->genericParams.empty()) {
                            sig += "<";
                            for (size_t g = 0; g < fn->genericParams.size(); ++g) {
                                if (fn->genericParams[g].isVariadic) sig += "variadic ";
                                sig += fn->genericParams[g].name;
                                if (g + 1 < fn->genericParams.size()) sig += ", ";
                            }
                            sig += ">";
                        }
                        sig += "(";
                        for (size_t p = 0; p < fn->params.size(); ++p) {
                            sig += fn->params[p].name + ": " + fn->params[p].parsedType.toString();
                            if (p + 1 < fn->params.size()) sig += ", ";
                        }
                        sig += "): " + fn->returnType.toString();
                        hoverMarkdown += makeCodeBlock(sig);
                        std::string meta;
                        meta += makeBullet("Kind", fn->genericParams.empty() ? "Function" : "Generic Function");
                        meta += makeBullet("Return Type", "`" + fn->returnType.toString() + "`");
                        hoverMarkdown += makeSection("Symbol Info", meta);
                        appendDocs(fn->loc);
                        resolved = true;
                    }
                    else if (!resSt.empty() || !resGenSt.empty()) {
                        StructDecl* st = !resSt.empty() ? currentSema->m_structs[resSt] : currentSema->m_generic_structs[resGenSt];
                        std::string code = "struct " + st->name;
                        if (!st->genericParams.empty()) {
                            code += "<";
                            for (size_t g = 0; g < st->genericParams.size(); ++g) {
                                code += st->genericParams[g].name;
                                if (g + 1 < st->genericParams.size()) code += ", ";
                            }
                            code += ">";
                        }
                        code += " {\n";
                        int totalSize = 0;
                        for (const auto& field : st->fields) {
                            Type* ft = currentSema->resolveType(field.parsedType);
                            int fSize = ft ? ft->sizeBytes : 8;
                            if (st->genericParams.empty()) totalSize = std::max(totalSize, field.offset + fSize);
                            code += "    " + field.name + ": " + field.parsedType.toString() + ";\n";
                        }
                        code += "}";
                        hoverMarkdown += makeCodeBlock(code);
                        std::string meta;
                        meta += makeBullet("Kind", st->genericParams.empty() ? "Struct" : "Generic Struct");
                        meta += makeBullet("Fields", std::to_string(st->fields.size()));
                        if (st->genericParams.empty()) meta += makeBullet("Footprint", "`" + std::to_string(totalSize) + " bytes`");
                        else meta += makeBullet("Footprint", "Dependent on generic arguments");
                        hoverMarkdown += makeSection("Symbol Info", meta);
                        appendDocs(st->loc);
                        resolved = true;
                    }
                    else if (!resEnm.empty()) {
                        auto enm = currentSema->m_enums[resEnm];
                        std::string code = "enum " + enm->name + " {\n";
                        for (const auto& m : enm->members) code += "    " + m.name + " = " + std::to_string(m.value) + ",\n";
                        code += "}";
                        hoverMarkdown += makeCodeBlock(code);
                        std::string meta;
                        meta += makeBullet("Kind", "Enum");
                        meta += makeBullet("Members", std::to_string(enm->members.size()));
                        hoverMarkdown += makeSection("Symbol Info", meta);
                        appendDocs(enm->loc);
                        resolved = true;
                    }
                    else if (!resAlias.empty()) {
                        auto target = currentSema->m_aliases[resAlias];
                        hoverMarkdown += makeCodeBlock("alias " + resAlias + " = " + target.toString());
                        std::string meta;
                        meta += makeBullet("Kind", "Alias");
                        meta += makeBullet("Resolves To", "`" + target.toString() + "`");
                        hoverMarkdown += makeSection("Symbol Info", meta);
                        resolved = true;
                    }
                }
                else if (!resolved && (currentSema->m_structs.count(scopedFqn) || currentSema->m_structs.count(fqn) ||
                    currentSema->m_generic_structs.count(scopedFqn) || currentSema->m_generic_structs.count(fqn))) {
                    StructDecl* st = nullptr;
                    if (currentSema->m_structs.count(scopedFqn)) st = currentSema->m_structs[scopedFqn];
                    else if (currentSema->m_structs.count(fqn)) st = currentSema->m_structs[fqn];
                    else if (currentSema->m_generic_structs.count(scopedFqn)) st = currentSema->m_generic_structs[scopedFqn];
                    else if (currentSema->m_generic_structs.count(fqn)) st = currentSema->m_generic_structs[fqn];

                    std::string code = "struct " + st->name;
                    if (!st->genericParams.empty()) {
                        code += "<";
                        for (size_t g = 0; g < st->genericParams.size(); ++g) {
                            code += st->genericParams[g].name;
                            if (g + 1 < st->genericParams.size()) code += ", ";
                        }
                        code += ">";
                    }
                    code += " {\n";

                    int totalSize = 0;
                    for (const auto& field : st->fields) {
                        Type* ft = currentSema->resolveType(field.parsedType);
                        int fSize = ft ? ft->sizeBytes : 8;
                        if (st->genericParams.empty()) {
                            totalSize = std::max(totalSize, field.offset + fSize);
                        }
                        code += "    " + field.name + ": " + field.parsedType.toString() + ";\n";
                    }
                    code += "}";
                    hoverMarkdown += makeCodeBlock(code);

                    std::string meta;
                    meta += makeBullet("Kind", st->genericParams.empty() ? "Struct" : "Generic Struct");
                    meta += makeBullet("Fields", std::to_string(st->fields.size()));

                    if (st->genericParams.empty()) {
                        meta += makeBullet("Footprint", "`" + std::to_string(totalSize) + " bytes`");
                    }
                    else {
                        meta += makeBullet("Footprint", "Dependent on generic arguments");
                    }

                    hoverMarkdown += makeSection("Symbol Info", meta);
                    appendDocs(st->loc);
                    resolved = true;
                }
                else if (!resolved && (currentSema->m_enums.count(scopedFqn) || currentSema->m_enums.count(fqn))) {
                    auto enm = currentSema->m_enums.count(scopedFqn) ? currentSema->m_enums[scopedFqn] : currentSema->m_enums[fqn];
                    std::string code = "enum " + enm->name + " {\n";
                    for (const auto& m : enm->members) code += "    " + m.name + " = " + std::to_string(m.value) + ",\n";
                    code += "}";
                    hoverMarkdown += makeCodeBlock(code);
                    std::string meta;
                    meta += makeBullet("Kind", "Enum");
                    meta += makeBullet("Members", std::to_string(enm->members.size()));
                    hoverMarkdown += makeSection("Symbol Info", meta);
                    appendDocs(enm->loc);
                    resolved = true;
                }
                else if (!resolved && (currentSema->m_aliases.count(scopedFqn) || currentSema->m_aliases.count(fqn))) {
                    std::string targetFqn = currentSema->m_aliases.count(scopedFqn) ? scopedFqn : fqn;
                    auto target = currentSema->m_aliases[targetFqn];
                    hoverMarkdown += makeCodeBlock("alias " + targetFqn + " = " + target.toString());
                    std::string meta;
                    meta += makeBullet("Kind", "Alias");
                    meta += makeBullet("Resolves To", "`" + target.toString() + "`");
                    hoverMarkdown += makeSection("Symbol Info", meta);
                    resolved = true;
                }

                if (!resolved && currentSema) {
                    for (const auto& [ename, enm] : currentSema->m_enums) {
                        for (const auto& member : enm->members) {
                            if (member.name == token.text) {
                                hoverMarkdown += makeCodeBlock(ename + "::" + member.name + " = " + std::to_string(member.value));
                                std::string meta;
                                meta += makeBullet("Kind", "Enum Member");
                                meta += makeBullet("Parent Enum", "`" + ename + "`");
                                meta += makeBullet("Value", "`" + std::to_string(member.value) + "`");
                                hoverMarkdown += makeSection("Symbol Info", meta);
                                appendDocs(enm->loc);
                                resolved = true;
                                break;
                            }
                        }
                        if (resolved) break;
                    }
                }

                if (!resolved && currentSema && !activeStruct.empty()) {
                    StructDecl* st = nullptr;
                    if (currentSema->m_structs.count(activeStruct)) st = currentSema->m_structs[activeStruct];
                    else if (currentSema->m_generic_structs.count(activeStruct)) st = currentSema->m_generic_structs[activeStruct];

                    if (st) {
                        for (auto& field : st->fields) {
                            if (field.name == token.text) {
                                Type* ft = currentSema->resolveType(field.parsedType);
                                int fSize = ft ? ft->sizeBytes : 8;

                                hoverMarkdown += makeCodeBlock(field.name + ": " + field.parsedType.toString());
                                std::string meta;
                                meta += makeBullet("Kind", "Struct Field (Definition)");
                                meta += makeBullet("Parent Type", "`" + activeStruct + "`");

                                if (st->genericParams.empty()) {
                                    meta += makeBullet("Offset", "`0x" + std::format("{:X}", field.offset) + "`");
                                    meta += makeBullet("Size", "`" + std::to_string(fSize) + " bytes`");
                                }
                                else {
                                    meta += makeBullet("Offset", "Dependent on generic arguments");
                                    meta += makeBullet("Size", "Dependent on generic arguments");
                                }

                                hoverMarkdown += makeSection("Field Info", meta);
                                appendDocs(st->loc);
                                resolved = true;
                                break;
                            }
                        }

                        if (!resolved) {
                            for (auto& gp : st->genericParams) {
                                if (gp.name == token.text) {
                                    hoverMarkdown += makeCodeBlock("generic " + gp.name);
                                    std::string meta;
                                    meta += makeBullet("Kind", "Generic Type Parameter");
                                    meta += makeBullet("Parent Type", "`" + activeStruct + "`");
                                    hoverMarkdown += makeSection("Symbol Info", meta);
                                    resolved = true;
                                    break;
                                }
                            }
                        }
                    }
                }

                if (!resolved && i >= 2 && tokens[i - 1].type == TokenType::Dot) {
                    auto searchGlobalField = [&](const auto& structMap) {
                        for (const auto& [sName, st] : structMap) {
                            for (auto& field : st->fields) {
                                if (field.name == token.text) {
                                    Type* ft = currentSema->resolveType(field.parsedType);
                                    int fSize = ft ? ft->sizeBytes : 8;

                                    hoverMarkdown += makeCodeBlock(field.name + ": " + field.parsedType.toString());
                                    std::string meta;
                                    meta += makeBullet("Kind", "Struct Field (Deduced from Generic)");
                                    meta += makeBullet("Possible Parent Type", "`" + sName + "`");

                                    if (st->genericParams.empty()) {
                                        meta += makeBullet("Offset", "`0x" + std::format("{:X}", field.offset) + "`");
                                        meta += makeBullet("Size", "`" + std::to_string(fSize) + " bytes`");
                                    }
                                    else {
                                        meta += makeBullet("Offset", "Dependent on generic arguments");
                                        meta += makeBullet("Size", "Dependent on generic arguments");
                                    }

                                    if (field.parsedType.hasModifier(TypeModifier::Owner))
                                        meta += makeBullet("Memory", "Owning heap pointer");
                                    else if (field.parsedType.hasModifier(TypeModifier::Ref))
                                        meta += makeBullet("Memory", "Borrowed reference");

                                    hoverMarkdown += makeSection("Field Info", meta);
                                    appendDocs(st->loc);
                                    resolved = true;
                                    return true;
                                }
                            }
                        }
                        return false;
                    };

                    if (!searchGlobalField(currentSema->m_structs)) {
                        searchGlobalField(currentSema->m_generic_structs);
                    }
                }

                if (!resolved) {
                    std::string varType = getVarType(token.text);
                    if (!varType.empty()) {
                        hoverMarkdown += makeCodeBlock(token.text + ": " + varType);
                        std::string meta;

                        meta += makeBullet("Kind", "Variable");
                        meta += makeBullet("Type", "`" + varType + "`");

                        if (varType.starts_with("owner ")) meta += makeBullet("Memory", "Owning heap pointer");
                        else if (varType.starts_with("ref ")) meta += makeBullet("Memory", "Borrowed reference");

                        hoverMarkdown += makeSection("Symbol Info", meta);
                        resolved = true;
                    }
                }

                if (!resolved && currentSema) {
                    bool isGenericParam = false;
                    std::string parentTypeOrFn = "";

                    for (const auto& [name, st] : currentSema->m_generic_structs) {
                        for (const auto& gp : st->genericParams) {
                            if (gp.name == token.text) {
                                isGenericParam = true;
                                parentTypeOrFn = name;
                                break;
                            }
                        }
                        if (isGenericParam) break;
                    }

                    if (!isGenericParam) {
                        for (const auto& [name, fn] : currentSema->m_generic_functions) {
                            for (const auto& gp : fn->genericParams) {
                                if (gp.name == token.text) {
                                    isGenericParam = true;
                                    parentTypeOrFn = name;
                                    break;
                                }
                            }
                            if (isGenericParam) break;
                        }
                    }

                    if (isGenericParam) {
                        hoverMarkdown += makeCodeBlock("generic " + token.text);
                        std::string meta;
                        meta += makeBullet("Kind", "Generic Type Parameter");
                        meta += makeBullet("Defined In", "`" + parentTypeOrFn + "`");
                        hoverMarkdown += makeSection("Symbol Info", meta);
                        resolved = true;
                    }
                }
            }

            if (hoverMarkdown.empty()) {
                if (token.type == TokenType::Identifier && token.text == "else") {
                    hoverMarkdown = makeCodeBlock("else") + makeSection("Keyword", "Fallback branch of an if-statement.");
                }
                else {
                    switch (token.type) {
                    case TokenType::Fn:
                        hoverMarkdown =
                            makeCodeBlock("fn")
                            + makeSection(
                                "Keyword",
                                "Declares a function."
                            );
                        break;

                    case TokenType::Struct:
                        hoverMarkdown =
                            makeCodeBlock("struct")
                            + makeSection(
                                "Keyword",
                                "Declares a contiguous composite type."
                            );
                        break;

                    case TokenType::Enum:
                        hoverMarkdown =
                            makeCodeBlock("enum")
                            + makeSection(
                                "Keyword",
                                "Declares an enumeration."
                            );
                        break;

                    case TokenType::Sizeof:
                        hoverMarkdown =
                            makeCodeBlock("sizeof<T>")
                            + makeSection(
                                "Builtin",
                                "Returns compile-time size in bytes."
                            );
                        break;
                    case TokenType::BuiltinAllocate:
                        hoverMarkdown = makeCodeBlock("__builtin_allocate(size: u64, align: u64) -> owner u8") + makeSection("Builtin", "Allocates aligned memory on the heap.");
                        break;
                    case TokenType::BuiltinMemfill:
                        hoverMarkdown = makeCodeBlock("__builtin_memfill(dest: ref u8, val: u8, size: u64) -> void") + makeSection("Builtin", "Fast intrinsic memory fill.");
                        break;
                    case TokenType::BuiltinMemcpy:
                        hoverMarkdown = makeCodeBlock("__builtin_memcpy(dest: ref u8, src: ref u8, size: u64) -> void") + makeSection("Builtin", "Fast intrinsic memory copy.");
                        break;
                    case TokenType::BuiltinTrap:
                        hoverMarkdown = makeCodeBlock("__builtin_trap() -> void") + makeSection("Builtin", "Emits a hardware breakpoint (int3).");
                        break;
                    case TokenType::BuiltinBswap:
                        hoverMarkdown = makeCodeBlock("__builtin_bswap*(val: T) -> T") + makeSection("Builtin", "Reverses the byte order of a 16, 32, or 64-bit integer.");
                        break;
                    case TokenType::BuiltinUnreachable:
                        hoverMarkdown = makeCodeBlock("__builtin_unreachable() -> void") + makeSection("Builtin", "Hints to the optimizer that this code path is dead.");
                        break;

                    default:
                        break;
                    }
                }
            }

            break;
        }

        json result = nullptr;

        if (!hoverMarkdown.empty()) {
            result = {
                {
                    "contents",
                    {
                        { "kind", "markdown" },
                        { "value", hoverMarkdown }
                    }
                }
            };
        }

        sendResponse({
            { "jsonrpc", "2.0" },
            { "id", msg["id"] },
            { "result", result }
        });
    }

    void LSPServer::sendResponse(const json& response) {
        std::string payload = response.dump();
        std::cout << "Content-Length: " << payload.length() << "\r\n\r\n" << payload << std::flush;
    }

    void LSPServer::sendNotification(const std::string& method, const json& params) {
        json notification = {
            {"jsonrpc", "2.0"},
            {"method", method},
            {"params", params}
        };
        sendResponse(notification);
    }

    void LSPServer::handleMessage(const json& msg) {
        if (!msg.contains("method")) return;
        std::string method = msg["method"];

        if (method == "initialize") handleInitialize(msg);
        else if (method == "textDocument/didOpen") handleDidOpen(msg);
        else if (method == "textDocument/didChange") handleDidChange(msg);
        else if (method == "textDocument/hover") handleHover(msg);
        else if (method == "textDocument/definition") handleDefinition(msg);
        else if (method == "textDocument/semanticTokens/full") handleSemanticTokens(msg);
        else if (method == "textDocument/completion") handleCompletion(msg);
    }

    void LSPServer::handleInitialize(const json& msg) {
        json capabilities = {
            { "textDocumentSync", 1 },
            { "hoverProvider", true },
            { "definitionProvider", true },
            { "completionProvider", {
                { "resolveProvider", false },
                { "triggerCharacters", { ".", ":" } }
            }},
            { "semanticTokensProvider", {
                { "legend", {
                    { "tokenTypes", {
                        "keyword",      // 0
                        "type",         // 1
                        "class",        // 2
                        "function",     // 3
                        "variable",     // 4
                        "number",       // 5
                        "operator",     // 6
                        "string",       // 7
                        "macro",        // 8
                        "comment",      // 9
                        "builtin",      // 10
                        "namespace",    // 11
                        "enumMember"    // 12
                    }},
                    { "tokenModifiers", {
                        "declaration",
                        "definition",
                        "readonly",
                        "unnecessary"
                    }}
                }},
                { "full", true }
            }}
        };

        sendResponse({
            { "jsonrpc", "2.0" },
            { "id", msg["id"] },
            { "result", {
                { "capabilities", capabilities }
            }}
        });
    }

    void LSPServer::handleCompletion(const json& msg) {
        int reqLine = msg["params"]["position"]["line"];
        int reqChar = msg["params"]["position"]["character"];
        std::string uri = msg["params"]["textDocument"]["uri"];

        auto* currentSema = documentSemas.count(uri) ? documentSemas[uri].get() : nullptr;
        auto* currentProgram = documentPrograms.count(uri) ? documentPrograms[uri].get() : nullptr;

        std::string text = documents[uri];
        std::string currentLine = "";
        int currentLineIdx = 0;
        std::istringstream iss(text);
        std::string line;
        while (std::getline(iss, line)) {
            if (currentLineIdx == reqLine) {
                currentLine = line.substr(0, reqChar);
                break;
            }
            currentLineIdx++;
        }

        json items = json::array();

        int i = currentLine.length() - 1;
        std::string typedPrefix = "";
        while (i >= 0 && (std::isalnum(currentLine[i]) || currentLine[i] == '_')) {
            typedPrefix = currentLine[i] + typedPrefix;
            i--;
        }

        if (i >= 0 && currentLine[i] == '.') {
            int j = i - 1;
            while (j >= 0 && std::isspace(currentLine[j])) j--;

            std::string varName = "";
            while (j >= 0 && (std::isalnum(currentLine[j]) || currentLine[j] == '_')) {
                varName = currentLine[j] + varName;
                j--;
            }

            if (varName == "target") {
                std::vector<std::pair<std::string, std::string>> targetProps = {
                    {"is_windows", "bool (True if target OS is Windows)"},
                    {"is_linux", "bool (True if target OS is Linux)"},
                    {"is_macos", "bool (True if target OS is macOS)"},
                    {"is_wasm", "bool (True if target OS is WebAssembly)"},
                    {"is_x86_64", "bool (True if target architecture is x86_64)"},
                    {"is_arm64", "bool (True if target architecture is ARM64)"},
                    {"is_wasm32", "bool (True if target architecture is WASM32)"},
                    {"is_little_endian", "bool (True if system uses Little Endian byte ordering)"},
                    {"is_big_endian", "bool (True if system uses Big Endian byte ordering)"},
                    {"pointer_size", "i64 (Size of memory addresses in bytes: 4 or 8)"}
                };

                for (const auto& prop : targetProps) {
                    items.push_back({
                        {"label", prop.first},
                        {"kind", 10},
                        {"detail", "compiler.target property"},
                        {"documentation", prop.second}
                   });
                }
            }

            if (!varName.empty() && varName != "target") {
                std::string typeName = "";
                size_t absPos = 0;
                int lineCnt = 0;
                for (size_t k = 0; k < text.length(); ++k) {
                    if (lineCnt == reqLine) { absPos = k + reqChar; break; }
                    if (text[k] == '\n') lineCnt++;
                }

                std::string searchPattern = varName + ":";
                size_t pos = text.rfind(searchPattern, absPos);
                if (pos != std::string::npos) {
                    size_t typeStart = pos + searchPattern.length();
                    while (typeStart < text.length() && (text[typeStart] == ' ' || text[typeStart] == '\t')) typeStart++;

                    int angleDepth = 0;
                    for (size_t k = typeStart; k < text.length(); ++k) {
                        char c = text[k];
                        if (c == '<') angleDepth++;
                        else if (c == '>') angleDepth--;
                        else if (angleDepth == 0 && (c == ' ' || c == ';' || c == '=' || c == '\n' || c == ',' || c == ')')) break;
                        typeName += c;
                    }
                }

                if (currentSema && typeName.empty()) {
                    std::string directCheck = varName;
                    while (currentSema->m_aliases.count(directCheck)) directCheck = currentSema->m_aliases[directCheck].baseName;
                    if (currentSema->m_structs.count(directCheck) || currentSema->m_generic_structs.count(directCheck)) {
                        typeName = directCheck;
                    }
                }

                if (currentProgram && typeName.empty()) {
                    for (const auto& fn : currentProgram->functions) {
                        for (const auto& p : fn->params) {
                            if (p.name == varName) typeName = p.parsedType.toString();
                        }
                        if (fn->body && typeName.empty()) {
                            std::function<void(const BlockStmt*)> searchBlock = [&](const BlockStmt* block) {
                                if (!block) return;
                                for (const auto& stmt : block->statements) {
                                    if (auto decl = dynamic_cast<VarDecl*>(stmt.get())) {
                                        if (decl->name == varName) typeName = decl->parsedType.toString();
                                    }
                                    else if (auto b = dynamic_cast<BlockStmt*>(stmt.get())) {
                                        searchBlock(b);
                                    }
                                    else if (auto i_stmt = dynamic_cast<IfStmt*>(stmt.get())) {
                                        searchBlock(dynamic_cast<BlockStmt*>(i_stmt->thenBranch.get()));
                                        if (i_stmt->elseBranch) searchBlock(dynamic_cast<BlockStmt*>(i_stmt->elseBranch.get()));
                                    }
                                    else if (auto w = dynamic_cast<WhileStmt*>(stmt.get())) {
                                        searchBlock(dynamic_cast<BlockStmt*>(w->body.get()));
                                    }
                                    else if (auto f = dynamic_cast<ForStmt*>(stmt.get())) {
                                        if (f->init) {
                                            if (auto decl = dynamic_cast<VarDecl*>(f->init.get())) {
                                                if (decl->name == varName) typeName = decl->parsedType.toString();
                                            }
                                        }
                                        searchBlock(dynamic_cast<BlockStmt*>(f->body.get()));
                                    }
                                }
                                };
                            searchBlock(fn->body.get());
                        }
                    }
                }

                typeName.erase(std::remove(typeName.begin(), typeName.end(), ' '), typeName.end());
                if (typeName.starts_with("ref")) typeName = typeName.substr(3);
                if (typeName.starts_with("owner")) typeName = typeName.substr(5);

                size_t bracket = typeName.find('[');
                if (bracket != std::string::npos) typeName = typeName.substr(0, bracket);

                size_t angleBracket = typeName.find('<');
                if (angleBracket != std::string::npos) typeName = typeName.substr(0, angleBracket);

                if (currentSema && !typeName.empty()) {
                    while (currentSema->m_aliases.count(typeName)) {
                        typeName = currentSema->m_aliases[typeName].baseName;
                    }

                    auto addFields = [&](const auto& structMap) {
                        if (structMap.count(typeName)) {
                            for (const auto& field : structMap.at(typeName)->fields) {
                                Type* ft = currentSema->resolveType(field.parsedType);
                                int fSize = ft ? ft->sizeBytes : 8;
                                items.push_back({
                                    {"label", field.name},
                                    {"kind", 5},
                                    {"detail", std::format("{} (Offset: {:#04x}, Size: {})", field.parsedType.toString(), field.offset, fSize)}
                                });
                            }
                        }
                        };
                    addFields(currentSema->m_structs);
                    addFields(currentSema->m_generic_structs);

                    std::string methodPrefix = typeName + "_";
                    std::string methodPrefix2 = typeName + "::";

                    auto addMethods = [&](const auto& funcMap) {
                        for (const auto& [fName, fnDecl] : funcMap) {
                            std::string methodName;

                            if (fName.starts_with(methodPrefix)) methodName = fName.substr(methodPrefix.length());
                            else if (fName.starts_with(methodPrefix2)) methodName = fName.substr(methodPrefix2.length());

                            if (!methodName.empty()) {
                                std::string sig = "fn(";
                                bool hasThis = !fnDecl->params.empty() && fnDecl->params[0].name == "this";
                                size_t startIdx = hasThis ? 1 : 0;
                                for (size_t pIdx = startIdx; pIdx < fnDecl->params.size(); ++pIdx) {
                                    sig += fnDecl->params[pIdx].name + ": " + fnDecl->params[pIdx].parsedType.toString();
                                    if (pIdx + 1 < fnDecl->params.size()) sig += ", ";
                                }
                                sig += ") -> " + fnDecl->returnType.toString();
                                items.push_back({
                                    {"label", methodName},
                                    {"kind", 2},
                                    {"detail", sig}
                                    });
                            }
                        }
                    };
                    addMethods(currentSema->m_functions);
                    addMethods(currentSema->m_generic_functions);
                }
            }
        }
        else if (i >= 1 && currentLine[i] == ':' && currentLine[i - 1] == ':') {
            int j = i - 2;
            std::string prefix = "";
            while (j >= 0 && (std::isalnum(currentLine[j]) || currentLine[j] == '_' || currentLine[j] == ':')) {
                prefix = currentLine[j] + prefix;
                j--;
            }

            if (currentSema) {
                std::string nsPrefix = prefix + "::";
                std::set<std::string> suggestions;

                auto addSuggestions = [&](const auto& map) {
                    for (const auto& [name, decl] : map) {
                        if (name.starts_with(nsPrefix)) {
                            std::string remainder = name.substr(nsPrefix.length());
                            size_t nextColon = remainder.find("::");
                            if (nextColon != std::string::npos) {
                                suggestions.insert(remainder.substr(0, nextColon));
                            }
                            else {
                                suggestions.insert(remainder);
                            }
                        }
                    }
                };

                addSuggestions(currentSema->m_functions);
                addSuggestions(currentSema->m_generic_functions);
                addSuggestions(currentSema->m_structs);
                addSuggestions(currentSema->m_generic_structs);
                addSuggestions(currentSema->m_enums);
                addSuggestions(currentSema->m_aliases);

                for (const auto& sug : suggestions) {
                    int kind = 14;
                    std::string detail = "namespace module";
                    std::string fullItem = nsPrefix + sug;

                    if (currentSema->m_functions.count(fullItem) || currentSema->m_generic_functions.count(fullItem)) { kind = 3; detail = "fn"; }
                    else if (currentSema->m_structs.count(fullItem) || currentSema->m_generic_structs.count(fullItem)) { kind = 7; detail = "struct"; }
                    else if (currentSema->m_enums.count(fullItem)) { kind = 13; detail = "enum"; }
                    else if (currentSema->m_aliases.count(fullItem)) { kind = 14; detail = "alias"; }

                    items.push_back({
                        {"label", sug},
                        {"kind", kind},
                        {"detail", detail}
                    });
                }

                if (currentSema->m_enums.count(prefix)) {
                    auto enm = currentSema->m_enums[prefix];
                    for (const auto& member : enm->members) {
                        items.push_back({
                            {"label", member.name},
                            {"kind", 20},
                            {"detail", "Value: " + std::to_string(member.value)}
                        });
                    }
                }

                auto tryAddMethods = [&](const auto& structMap) {
                    if (structMap.count(prefix)) {
                        std::string methodPrefix = prefix + "_";
                        std::string methodPrefix2 = prefix + "::";

                        auto addStaticMethods = [&](const auto& funcMap) {
                            for (const auto& [fName, fnDecl] : funcMap) {
                                std::string methodName;

                                if (fName.starts_with(methodPrefix)) methodName = fName.substr(methodPrefix.length());
                                else if (fName.starts_with(methodPrefix2)) methodName = fName.substr(methodPrefix2.length());

                                if (!methodName.empty()) {
                                    std::string sig = "fn(";
                                    for (size_t pIdx = 0; pIdx < fnDecl->params.size(); ++pIdx) {
                                        sig += fnDecl->params[pIdx].name + ": " + fnDecl->params[pIdx].parsedType.toString();
                                        if (pIdx + 1 < fnDecl->params.size()) sig += ", ";
                                    }
                                    sig += ") -> " + fnDecl->returnType.toString();
                                    items.push_back({
                                        {"label", methodName},
                                        {"kind", 3},
                                        {"detail", sig}
                                    });
                                }
                            }
                        };
                        addStaticMethods(currentSema->m_functions);
                        addStaticMethods(currentSema->m_generic_functions);
                    }
                };
                tryAddMethods(currentSema->m_structs);
                tryAddMethods(currentSema->m_generic_structs);
            }
        }
        else {
            std::set<std::string> addedVars;
            size_t scanPos = 0;
            while ((scanPos = text.find(':', scanPos)) != std::string::npos) {
                if (scanPos > 0 && scanPos < text.length() - 1 && text[scanPos + 1] != ':') {
                    size_t nameEnd = scanPos - 1;
                    while (nameEnd > 0 && (text[nameEnd] == ' ' || text[nameEnd] == '\t')) nameEnd--;
                    if (std::isalnum(text[nameEnd]) || text[nameEnd] == '_') {
                        size_t nameStart = nameEnd;
                        while (nameStart > 0 && (std::isalnum(text[nameStart - 1]) || text[nameStart - 1] == '_')) nameStart--;
                        std::string foundVar = text.substr(nameStart, nameEnd - nameStart + 1);

                        if (foundVar != "fn" && foundVar != "struct" && foundVar != "enum" && foundVar != "alias") {
                            if (addedVars.insert(foundVar).second) {
                                items.push_back({ {"label", foundVar}, {"kind", 6}, {"detail", "local variable"} });
                            }
                        }
                    }
                }
                scanPos++;
            }

            if (currentSema) {
                auto addGlobalFuncs = [&](const auto& funcMap, const auto& structMap) {
                    for (const auto& [name, fn] : funcMap) {
                        if (name.find("_") != std::string::npos && structMap.count(name.substr(0, name.find("_")))) continue;

                        std::string sig = "fn(";
                        for (size_t j = 0; j < fn->params.size(); ++j) {
                            sig += fn->params[j].parsedType.toString();
                            if (j + 1 < fn->params.size()) sig += ", ";
                        }
                        sig += ") -> " + fn->returnType.toString();
                        items.push_back({ {"label", name}, {"kind", 3}, {"detail", sig} });
                    }
                };

                addGlobalFuncs(currentSema->m_functions, currentSema->m_structs);
                addGlobalFuncs(currentSema->m_generic_functions, currentSema->m_generic_structs);

                for (const auto& [name, st] : currentSema->m_structs) {
                    items.push_back({ {"label", name}, {"kind", 7}, {"detail", "struct (" + std::to_string(st->fields.size()) + " fields)"} });
                }
                for (const auto& [name, st] : currentSema->m_generic_structs) {
                    items.push_back({ {"label", name}, {"kind", 7}, {"detail", "generic struct (" + std::to_string(st->genericParams.size()) + " params, " + std::to_string(st->fields.size()) + " fields)"} });
                }
                for (const auto& [name, enm] : currentSema->m_enums) {
                    items.push_back({ {"label", name}, {"kind", 13}, {"detail", "enum"} });
                }
                for (const auto& [name, type] : currentSema->m_aliases) {
                    items.push_back({ {"label", name}, {"kind", 14}, {"detail", "alias -> " + type.toString()} });
                }
            }

            std::vector<std::pair<std::string, std::string>> environmentMacros = {
                {"__WIN32", "Deprecated: Use compiler.target.is_windows instead"},
                {"__WIN64", "Deprecated: Use compiler.target.is_windows && compiler.target.pointer_size == 8 instead"},
                {"__LINUX", "Deprecated: Use compiler.target.is_linux instead"},
                {"__MACOS", "Deprecated: Use compiler.target.is_macos instead"},
                {"__WASM", "Deprecated: Use compiler.target.is_wasm instead"},
                {"__X86_64", "Deprecated: Use compiler.target.is_x86_64 instead"},
                {"__ARM64", "Deprecated: Use compiler.target.is_arm64 instead"},
                {"__SYSV", "Deprecated: Use System V calling convention check rules instead"},
                {"__LITTLE_ENDIAN__", "Deprecated: Use compiler.target.is_little_endian instead"},
                {"__BIG_ENDIAN__", "Deprecated: Use compiler.target.is_big_endian instead"},
                {"__PTR_SIZE__", "Deprecated: Use compiler.target.pointer_size instead"},
                {"compiler", "Built-in compiler intrinsic gateway namespace"}
            };

            for (const auto& macro : environmentMacros) {
                items.push_back({
                    {"label", macro.first},
                    {"kind", 25},
                    {"detail", macro.second}
                });
            }

            std::vector<std::string> keywords = {
                "fn", "struct", "enum", "alias", "return", "if", "else", "while", "u8", "u16", "u32", "u64",
                "i8", "i16", "i32", "i64", "f32", "f64", "void", "ref", "owner", "alloc", "sizeof", "cast", "cast_bits", "namespace", "for", "true", "false", "null"
            };
            for (const auto& kw : keywords) {
                items.push_back({ {"label", kw}, {"kind", 14} });
            }
            std::vector<std::pair<std::string, std::string>> builtins = {
                {"__builtin_allocate", "fn(size: u64, align: u64) -> owner u8"},
                {"__builtin_memfill", "fn(dest: ref u8, val: u8, size: u64) -> void"},
                {"__builtin_memcpy", "fn(dest: ref u8, src: ref u8, size: u64) -> void"},
                {"__builtin_trap", "fn() -> void"},
                {"__builtin_bswap16", "fn(val: u16) -> u16"},
                {"__builtin_bswap32", "fn(val: u32) -> u32"},
                {"__builtin_bswap64", "fn(val: u64) -> u64"},
                {"__builtin_unreachable", "fn() -> void"}
            };

            for (const auto& builtin : builtins) {
                items.push_back({
                    {"label", builtin.first},
                    {"kind", 3},
                    {"detail", builtin.second}
                });
            }
        }

        sendResponse({
            {"jsonrpc", "2.0"},
            {"id", msg["id"]},
            {"result", items}
        });
    }

    void LSPServer::handleDidOpen(const json& msg) {
        std::string uri = msg["params"]["textDocument"]["uri"];
        std::string text = msg["params"]["textDocument"]["text"];
        documents[uri] = text;
        dirtyUri = uri;
        needsCompilation = true;
        compileDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    }

    void LSPServer::handleDidChange(const json& msg) {
        std::string uri = msg["params"]["textDocument"]["uri"];
        std::string text = msg["params"]["contentChanges"][0]["text"];
        documents[uri] = text;
        dirtyUri = uri;
        needsCompilation = true;
        compileDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    }

    void LSPServer::compileAndPublishDiagnostics(const std::string& uri, const std::string& text) {
        Parser::m_comptimeVars.clear();
        Parser::m_lockedVars.clear();
        importTokenCache.clear();

        json diagnostics = json::array();

        try {
            Lexer lexer(text, uri);
            auto tokens = lexer.tokenize();
            Parser parser(std::move(tokens));
            auto program = parser.parse();

            if (parser.hasErrors) {
                for (std::string errStr : parser.errors) {
                    int severity = 1;
                    if (errStr.starts_with("Warning: ")) {
                        severity = 2;
                        errStr = errStr.substr(9);
                    }

                    int line = 0, col = 0;
                    if (errStr.starts_with("Line ")) {
                        size_t colonPos = errStr.find(':');
                        size_t dashPos = errStr.find(" - ");
                        if (colonPos != std::string::npos && dashPos != std::string::npos) {
                            try {
                                line = std::stoi(errStr.substr(5, colonPos - 5)) - 1;
                                col = std::stoi(errStr.substr(colonPos + 1, dashPos - colonPos - 1)) - 1;
                                errStr = errStr.substr(dashPos + 3);
                            }
                            catch (...) {}
                        }
                    }
                    line = std::max(0, line);
                    col = std::max(0, col);
                    diagnostics.push_back({
                        {"severity", severity},
                        {"range", {
                            {"start", {{"line", line}, {"character", col}}},
                            {"end", {{"line", line}, {"character", col + 5}}}
                        }},
                        {"message", errStr},
                        {"source", "gbpp-parser"}
                    });
                }
            }

            if (program) {
                documentPrograms[uri] = std::move(program);

                try {
                    loadImportsRecursively(documentPrograms[uri].get(), uri, importTokenCache);
                }
                catch (const std::exception& e) {
                    diagnostics.push_back({
                        {"severity", 1},
                        {"range", {
                            {"start", {{"line", 0}, {"character", 0}}},
                            {"end", {{"line", 0}, {"character", 5}}}
                        }},
                        {"message", e.what()},
                        {"source", "gbpp-imports"}
                    });
                }

                auto analyzer = std::make_unique<Sema>();
                analyzer->analyze(*documentPrograms[uri]);
                documentSemas[uri] = std::move(analyzer);

                auto* currentProgram = documentPrograms[uri].get();
                auto* currentSema = documentSemas[uri].get();

                for (std::string errStr : currentSema->errors) {
                    int severity = 1;
                    if (errStr.starts_with("Warning: ")) {
                        severity = 2;
                        errStr = errStr.substr(9);
                    }

                    json relatedInfo = json::array();
                    size_t relPos = errStr.find("\t[REL|");
                    if (relPos != std::string::npos) {
                        size_t endPos = errStr.find("]", relPos);
                        if (endPos != std::string::npos) {
                            std::string relData = errStr.substr(relPos + 6, endPos - (relPos + 6));
                            errStr = errStr.substr(0, relPos);

                            size_t p1 = relData.find('|');
                            if (p1 != std::string::npos) {
                                std::string rUri = relData.substr(0, p1);
                                size_t p2 = relData.find('|', p1 + 1);
                                if (p2 != std::string::npos) {
                                    int rLine = std::max(0, std::stoi(relData.substr(p1 + 1, p2 - p1 - 1)) - 1);
                                    size_t p3 = relData.find('|', p2 + 1);
                                    if (p3 != std::string::npos) {
                                        int rCol = std::max(0, std::stoi(relData.substr(p2 + 1, p3 - p2 - 1)) - 1);
                                        std::string rMsg = relData.substr(p3 + 1);
                                        relatedInfo.push_back({
                                            {"location", {
                                                {"uri", rUri},
                                                {"range", {
                                                    {"start", {{"line", rLine}, {"character", rCol}}},
                                                    {"end", {{"line", rLine}, {"character", rCol + rMsg.length()}}}
                                                }}
                                            }},
                                            {"message", "Definition of " + rMsg}
                                        });
                                    }
                                }
                            }
                        }
                    }

                    int line = 0, col = 0;
                    if (errStr.starts_with("Line ")) {
                        size_t colonPos = errStr.find(':');
                        size_t dashPos = errStr.find(" - ");
                        if (colonPos != std::string::npos && dashPos != std::string::npos) {
                            try {
                                line = std::stoi(errStr.substr(5, colonPos - 5)) - 1;
                                col = std::stoi(errStr.substr(colonPos + 1, dashPos - colonPos - 1)) - 1;
                                errStr = errStr.substr(dashPos + 3);
                            }
                            catch (...) {}
                        }
                    }
                    line = std::max(0, line);
                    col = std::max(0, col);

                    json diag = {
                        {"severity", severity},
                        {"range", {
                            {"start", {{"line", line}, {"character", col}}},
                            {"end", {{"line", line}, {"character", col + 5}}}
                        }},
                        {"message", errStr},
                        {"source", "gbpp-analyzer"}
                    };

                    if (!relatedInfo.empty()) {
                        diag["relatedInformation"] = relatedInfo;
                    }
                    diagnostics.push_back(diag);
                }

                for (const auto& fn : currentProgram->functions) {
                    if (fn->body) {
                        for (const auto& stmt : fn->body->statements) {
                            if (auto decl = dynamic_cast<VarDecl*>(stmt.get())) {
                                
                            }
                        }
                    }
                }
            }
        }
        catch (const std::exception& e) {
            std::string errStr = e.what();
            int severity = 1;

            if (errStr.starts_with("Warning: ")) {
                severity = 2;
                errStr = errStr.substr(9);
            }

            int line = 0, col = 0;
            if (errStr.starts_with("Line ")) {
                size_t colonPos = errStr.find(':');
                size_t dashPos = errStr.find(" - ");
                if (colonPos != std::string::npos && dashPos != std::string::npos) {
                    try {
                        line = std::stoi(errStr.substr(5, colonPos - 5)) - 1;
                        col = std::stoi(errStr.substr(colonPos + 1, dashPos - colonPos - 1)) - 1;
                        errStr = errStr.substr(dashPos + 3);
                    }
                    catch (...) {}
                }
            }
            line = std::max(0, line);
            col = std::max(0, col);
            diagnostics.push_back({
                {"severity", severity},
                {"range", {
                    {"start", {{"line", line}, {"character", col}}},
                    {"end", {{"line", line}, {"character", col + 5}}}
                }},
                {"message", errStr},
                {"source", "gbpp"}
            });
        }
        sendNotification("textDocument/publishDiagnostics", {
            {"uri", uri},
            {"diagnostics", diagnostics}
        });
    }

    void LSPServer::handleDefinition(const json& msg) {
        int reqLine = msg["params"]["position"]["line"];
        int reqChar = msg["params"]["position"]["character"];
        std::string uri = msg["params"]["textDocument"]["uri"];

        auto* currentSema = documentSemas.count(uri) ? documentSemas[uri].get() : nullptr;
        auto* currentProgram = documentPrograms.count(uri) ? documentPrograms[uri].get() : nullptr;

        Lexer lexer(documents[uri], uri);
        auto tokens = lexer.tokenize();

        json result = nullptr;
        std::vector<std::pair<std::string, int>> nsStack;
        std::string currentNamespace = "";
        int braceDepth = 0;

        std::string activeStruct = "";
        int structBraceDepth = -1;

        for (size_t i = 0; i < tokens.size(); ++i) {
            const auto& token = tokens[i];

            if (token.type == TokenType::Struct && i + 1 < tokens.size() && tokens[i + 1].type == TokenType::Identifier) {
                activeStruct = tokens[i + 1].text;
            }

            if (token.type == TokenType::LBrace) {
                braceDepth++;
                if (!activeStruct.empty() && structBraceDepth == -1) {
                    structBraceDepth = braceDepth;
                }
            }
            else if (token.type == TokenType::RBrace) {
                if (structBraceDepth == braceDepth) {
                    activeStruct = "";
                    structBraceDepth = -1;
                }
                braceDepth--;
                if (!nsStack.empty() && nsStack.back().second == braceDepth) {
                    nsStack.pop_back();
                    currentNamespace = "";
                    for (const auto& ns : nsStack) currentNamespace += (currentNamespace.empty() ? "" : "::") + ns.first;
                }
            }
            else if (token.type == TokenType::RBrace) {
                braceDepth--;
                if (!nsStack.empty() && nsStack.back().second == braceDepth) {
                    nsStack.pop_back();
                    currentNamespace = "";
                    for (const auto& ns : nsStack) currentNamespace += (currentNamespace.empty() ? "" : "::") + ns.first;
                }
            }
            else if (token.type == TokenType::Namespace && i + 1 < tokens.size() && tokens[i + 1].type == TokenType::Identifier) {
                std::string nsName = tokens[i + 1].text;
                nsStack.push_back({ nsName, braceDepth });
                currentNamespace = currentNamespace.empty() ? nsName : currentNamespace + "::" + nsName;
            }

            if (token.loc.line - 1 == reqLine && reqChar >= token.loc.col - 1 && reqChar <= token.loc.col - 1 + token.text.length()) {

                if (token.type == TokenType::Identifier && currentSema) {
                    SourceLoc targetLoc = { "", 0, 0 };

                    int l = i;
                    while (l >= 2 && tokens[l - 1].type == TokenType::DoubleColon) l -= 2;

                    std::string fqn = "";
                    for (int k = l; k <= i; ++k) fqn += tokens[k].text;

                    std::string scopedFqn = currentNamespace.empty() ? fqn : currentNamespace + "::" + fqn;

                    auto cleanTypeName = [&](std::string tName) {
                        if (tName.starts_with("ref ")) tName = tName.substr(4);
                        if (tName.starts_with("owner ")) tName = tName.substr(6);

                        size_t bracket = tName.find('[');
                        if (bracket != std::string::npos) tName = tName.substr(0, bracket);

                        size_t angleBracket = tName.find('<');
                        if (angleBracket != std::string::npos) tName = tName.substr(0, angleBracket);

                        while (currentSema && currentSema->m_aliases.count(tName)) tName = currentSema->m_aliases[tName].baseName;
                        return tName;
                    };

                    auto getVarType = [&](const std::string& vName) {
                        if (!currentProgram) return std::string("");
                        for (const auto& fn : currentProgram->functions) {
                            for (const auto& p : fn->params) { if (p.name == vName) return p.parsedType.toString(); }
                            if (fn->body) {
                                for (const auto& stmt : fn->body->statements) {
                                    if (auto decl = dynamic_cast<VarDecl*>(stmt.get())) {
                                        if (decl->name == vName) return decl->parsedType.toString();
                                    }
                                }
                            }
                        }
                        return std::string("");
                    };

                    auto resolveDotChain = [&](int tokenIdx) -> std::pair<StructDecl*, StructDecl::Field*> {
                        if (!currentSema)
                            return { nullptr, nullptr };

                        std::vector<std::string> chain;
                        int idx = tokenIdx;

                        while (idx >= 0 && tokens[idx].type == TokenType::Identifier) {
                            chain.push_back(tokens[idx].text);
                            if (idx >= 2 && tokens[idx - 1].type == TokenType::Dot)
                                idx -= 2;
                            else
                                break;
                        }

                        std::reverse(chain.begin(), chain.end());

                        if (chain.size() < 2)
                            return { nullptr, nullptr };

                        std::string currentTypeName = cleanTypeName(getVarType(chain[0]));
                        if (currentTypeName.empty())
                            return { nullptr, nullptr };

                        std::string resolvedType = findInLspMap(currentSema->m_structs, currentTypeName, currentNamespace);
                        if (resolvedType.empty()) resolvedType = findInLspMap(currentSema->m_generic_structs, currentTypeName, currentNamespace);
                        if (!resolvedType.empty()) currentTypeName = resolvedType;

                        StructDecl* currentStruct = nullptr;
                        StructDecl::Field* targetField = nullptr;

                        for (size_t k = 1; k < chain.size(); ++k) {
                            if (currentSema->m_structs.count(currentTypeName)) {
                                currentStruct = currentSema->m_structs[currentTypeName];
                            }
                            else if (currentSema->m_generic_structs.count(currentTypeName)) {
                                currentStruct = currentSema->m_generic_structs[currentTypeName];
                            }
                            else {
                                return { nullptr, nullptr };
                            }

                            bool foundField = false;

                            for (auto& field : currentStruct->fields) {

                                if (field.name == chain[k]) {
                                    targetField = &field;
                                    currentTypeName = cleanTypeName(field.parsedType.toString());
                                    foundField = true;
                                    break;
                                }
                            }

                            if (!foundField) return { nullptr, nullptr };
                        }
                        return { currentStruct, targetField };
                    };

                    std::string methodFullName = "";
                    if (i >= 2 && tokens[i - 1].type == TokenType::DoubleColon) {
                        int baseIdx = i - 2;
                        if (baseIdx >= 0 && tokens[baseIdx].type == TokenType::GT) {
                            int depth = 1;
                            baseIdx--;
                            while (baseIdx >= 0 && depth > 0) {
                                if (tokens[baseIdx].type == TokenType::GT) depth++;
                                else if (tokens[baseIdx].type == TokenType::LT) depth--;
                                baseIdx--;
                            }
                        }
                        std::string parentFqn = (baseIdx >= 0 && tokens[baseIdx].type == TokenType::Identifier) ? tokens[baseIdx].text : "";
                        if (!parentFqn.empty() && currentSema && (currentSema->m_structs.count(parentFqn) || currentSema->m_generic_structs.count(parentFqn))) {
                            if (currentSema->m_functions.count(parentFqn + "::" + token.text) || currentSema->m_generic_functions.count(parentFqn + "::" + token.text)) {
                                methodFullName = parentFqn + "::" + token.text;
                            }
                            else {
                                methodFullName = parentFqn + "_" + token.text;
                            }
                        }
                    }
                    else if (i >= 2 && tokens[i - 1].type == TokenType::Dot) {
                        auto [parentStruct, targetField] = resolveDotChain(i);
                        if (parentStruct && targetField) {
                            targetLoc = parentStruct->loc;
                        }
                        else {
                            auto [pStruct, pField] = resolveDotChain(i - 2);
                            std::string vType = pField ? cleanTypeName(pField->parsedType.toString()) : cleanTypeName(getVarType(tokens[i - 2].text));
                            if (!vType.empty()) {
                                if (currentSema->m_functions.count(vType + "::" + token.text) || currentSema->m_generic_functions.count(vType + "::" + token.text)) {
                                    methodFullName = vType + "::" + token.text;
                                }
                                else {
                                    methodFullName = vType + "_" + token.text;
                                }
                            }
                        }
                    }

                    if (targetLoc.line == 0) {
                        if (!methodFullName.empty() && (currentSema->m_functions.count(methodFullName) || currentSema->m_generic_functions.count(methodFullName))) {
                            targetLoc = currentSema->m_functions.count(methodFullName) ? currentSema->m_functions[methodFullName]->loc : currentSema->m_generic_functions[methodFullName]->loc;
                        }
                        else if (currentSema->m_functions.count(scopedFqn) || currentSema->m_functions.count(fqn) ||
                            currentSema->m_generic_functions.count(scopedFqn) || currentSema->m_generic_functions.count(fqn)) {
                            if (currentSema->m_functions.count(scopedFqn)) targetLoc = currentSema->m_functions[scopedFqn]->loc;
                            else if (currentSema->m_functions.count(fqn)) targetLoc = currentSema->m_functions[fqn]->loc;
                            else if (currentSema->m_generic_functions.count(scopedFqn)) targetLoc = currentSema->m_generic_functions[scopedFqn]->loc;
                            else if (currentSema->m_generic_functions.count(fqn)) targetLoc = currentSema->m_generic_functions[fqn]->loc;
                        }
                        else if (currentSema->m_structs.count(scopedFqn) || currentSema->m_structs.count(fqn) ||
                            currentSema->m_generic_structs.count(scopedFqn) || currentSema->m_generic_structs.count(fqn)) {
                            if (currentSema->m_structs.count(scopedFqn)) targetLoc = currentSema->m_structs[scopedFqn]->loc;
                            else if (currentSema->m_structs.count(fqn)) targetLoc = currentSema->m_structs[fqn]->loc;
                            else if (currentSema->m_generic_structs.count(scopedFqn)) targetLoc = currentSema->m_generic_structs[scopedFqn]->loc;
                            else if (currentSema->m_generic_structs.count(fqn)) targetLoc = currentSema->m_generic_structs[fqn]->loc;
                        }
                        else if (currentSema->m_enums.count(scopedFqn) || currentSema->m_enums.count(fqn)) {
                            targetLoc = currentSema->m_enums.count(scopedFqn) ? currentSema->m_enums[scopedFqn]->loc : currentSema->m_enums[fqn]->loc;
                        }
                        else if (currentSema->m_aliases.count(scopedFqn) || currentSema->m_aliases.count(fqn)) {
                            std::string targetFqn = currentSema->m_aliases.count(scopedFqn) ? scopedFqn : fqn;
                            for (const auto& alias : currentProgram->aliases) {
                                if (alias->name == targetFqn) { targetLoc = alias->loc; break; }
                            }
                        }
                        if (targetLoc.line == 0 && !activeStruct.empty()) {
                            std::string internalMethod1 = activeStruct + "::" + token.text;
                            std::string internalMethod2 = activeStruct + "_" + token.text;

                            if (currentSema->m_functions.count(internalMethod1)) targetLoc = currentSema->m_functions[internalMethod1]->loc;
                            else if (currentSema->m_generic_functions.count(internalMethod1)) targetLoc = currentSema->m_generic_functions[internalMethod1]->loc;
                            else if (currentSema->m_functions.count(internalMethod2)) targetLoc = currentSema->m_functions[internalMethod2]->loc;
                            else if (currentSema->m_generic_functions.count(internalMethod2)) targetLoc = currentSema->m_generic_functions[internalMethod2]->loc;

                            if (targetLoc.line == 0) {
                                StructDecl* st = nullptr;
                                if (currentSema->m_structs.count(activeStruct)) st = currentSema->m_structs[activeStruct];
                                else if (currentSema->m_generic_structs.count(activeStruct)) st = currentSema->m_generic_structs[activeStruct];
                                if (st) {
                                    for (auto& field : st->fields) {
                                        if (field.name == token.text) { targetLoc = st->loc; break; }
                                    }
                                }
                            }
                        }
                        else if (currentProgram) {
                            for (const auto& fn : currentProgram->functions) {
                                for (const auto& p : fn->params) {
                                    if (p.name == token.text) { targetLoc = fn->loc; break; }
                                }
                                if (fn->body && targetLoc.line == 0) {
                                    for (const auto& stmt : fn->body->statements) {
                                        if (auto decl = dynamic_cast<VarDecl*>(stmt.get())) {
                                            if (decl->name == token.text) {
                                                targetLoc = decl->loc; break;
                                            }
                                        }
                                    }
                                }
                                if (targetLoc.line != 0) break;
                            }
                        }

                        if (targetLoc.line == 0 && currentSema) {
                            for (const auto& [ename, enm] : currentSema->m_enums) {
                                for (const auto& member : enm->members) {
                                    if (member.name == token.text) {
                                        targetLoc = enm->loc;
                                        break;
                                    }
                                }
                                if (targetLoc.line != 0) break;
                            }
                        }

                        if (targetLoc.line == 0 && i >= 2 && tokens[i - 1].type == TokenType::Dot) {
                            auto searchGlobalField = [&](const auto& structMap) {
                                for (const auto& [sName, st] : structMap) {
                                    for (auto& field : st->fields) {
                                        if (field.name == token.text) {
                                            targetLoc = st->loc;
                                            return true;
                                        }
                                    }
                                }
                                return false;
                                };

                            if (!searchGlobalField(currentSema->m_structs)) {
                                searchGlobalField(currentSema->m_generic_structs);
                            }
                        }

                        if (targetLoc.line == 0 && currentSema) {
                            auto searchGenerics = [&](const auto& map) {
                                for (const auto& [name, decl] : map) {
                                    for (const auto& gp : decl->genericParams) {
                                        if (gp.name == token.text) {
                                            targetLoc = decl->loc;
                                            return true;
                                        }
                                    }
                                }
                                return false;
                                };

                            if (!searchGenerics(currentSema->m_generic_structs)) {
                                searchGenerics(currentSema->m_generic_functions);
                            }
                        }
                    }

                    if (targetLoc.line != 0) {
                        std::string targetUri = targetLoc.filename;
                        if (!targetUri.starts_with("file://")) {
                            std::replace(targetUri.begin(), targetUri.end(), '\\', '/');
                            if (targetUri.length() >= 2 && targetUri[1] == ':') {
                                targetUri = "/" + targetUri;
                            }
                            targetUri = "file://" + targetUri;
                        }

                        result = {
                            {"uri", targetUri},
                            {"range", {
                                {"start", {{"line", targetLoc.line - 1}, {"character", targetLoc.col - 1}}},
                                {"end", {{"line", targetLoc.line - 1}, {"character", targetLoc.col - 1 + token.text.length()}}}
                            }}
                        };
                    }
                }
                break;
            }
        }

        sendResponse({
            {"jsonrpc", "2.0"},
            {"id", msg["id"]},
            {"result", result}
        });
    }

    void LSPServer::handleSemanticTokens(const json& msg) {
        std::string uri = msg["params"]["textDocument"]["uri"];
        Lexer lexer(documents[uri], uri);
        auto tokens = lexer.tokenize();
        std::vector<int> data;
        int prevLine = 0;
        int prevChar = 0;
        bool inMacro = false;
        bool nextIsConstructorOffset = false;

        auto* currentSema = documentSemas.count(uri) ? documentSemas[uri].get() : nullptr;
        std::vector<std::pair<std::string, int>> nsStack;
        std::string currentNamespace = "";
        int braceDepth = 0;
        bool nextIdentIsConfig = false;

        std::unordered_map<std::string, bool> localComptimeVars;
        int inactiveBraceDepth = -1;
        bool inInactiveBlock = false;
        bool waitingForInactiveBrace = false;

        std::map<int, bool> comptimeChainTaken;
        std::map<int, bool> isComptimeBlock;
        bool lastClosedBlockWasComptime = false;
        bool nextBlockIsComptime = false;

        auto* currentProgram = documentPrograms.count(uri) ? documentPrograms[uri].get() : nullptr;
        std::set<std::string> configSymbols;

        if (currentProgram) {
            for (const auto& var : currentProgram->globalVars) {
                if (hasAttribute(var->attributes, AttrKind::Config)) configSymbols.insert(var->name);
            }
            for (const auto& fn : currentProgram->functions) {
                if (hasAttribute(fn->attributes, AttrKind::Config)) configSymbols.insert(fn->name);
                for (const auto& p : fn->params) {
                    if (hasAttribute(p.attributes, AttrKind::Config)) configSymbols.insert(p.name);
                }
                if (fn->body) {
                    std::function<void(const BlockStmt*)> scanBlock = [&](const BlockStmt* b) {
                        if (!b) return;
                        for (const auto& stmt : b->statements) {
                            if (auto v = dynamic_cast<VarDecl*>(stmt.get())) {
                                if (hasAttribute(v->attributes, AttrKind::Config)) configSymbols.insert(v->name);
                            }
                            else if (auto bs = dynamic_cast<BlockStmt*>(stmt.get())) {
                                scanBlock(bs);
                            }
                            else if (auto is = dynamic_cast<IfStmt*>(stmt.get())) {
                                scanBlock(dynamic_cast<BlockStmt*>(is->thenBranch.get()));
                                if (is->elseBranch) scanBlock(dynamic_cast<BlockStmt*>(is->elseBranch.get()));
                            }
                            else if (auto ws = dynamic_cast<WhileStmt*>(stmt.get())) {
                                scanBlock(dynamic_cast<BlockStmt*>(ws->body.get()));
                            }
                            else if (auto fs = dynamic_cast<ForStmt*>(stmt.get())) {
                                if (fs->init) {
                                    if (auto v = dynamic_cast<VarDecl*>(fs->init.get())) {
                                        if (hasAttribute(v->attributes, AttrKind::Config)) configSymbols.insert(v->name);
                                    }
                                }
                                scanBlock(dynamic_cast<BlockStmt*>(fs->body.get()));
                            }
                        }
                        };
                    scanBlock(fn->body.get());
                }
            }
            for (const auto& st : currentProgram->structs) {
                if (hasAttribute(st->attributes, AttrKind::Config)) configSymbols.insert(st->name);
                for (const auto& f : st->fields) {
                    if (hasAttribute(f.attributes, AttrKind::Config)) configSymbols.insert(f.name);
                }
            }
            for (const auto& en : currentProgram->enums) {
                if (hasAttribute(en->attributes, AttrKind::Config)) configSymbols.insert(en->name);
            }
            for (const auto& al : currentProgram->aliases) {
                if (hasAttribute(al->attributes, AttrKind::Config)) configSymbols.insert(al->name);
            }
        }

        std::set<std::string> knownGenericParams;
        if (currentSema) {
            for (const auto& [name, st] : currentSema->m_generic_structs) {
                for (const auto& gp : st->genericParams) knownGenericParams.insert(gp.name);
            }
            for (const auto& [name, fn] : currentSema->m_generic_functions) {
                for (const auto& gp : fn->genericParams) knownGenericParams.insert(gp.name);
            }
        }

        std::string activeStructForSemantics = "";
        int structBraceDepthForSemantics = -1;
        std::set<std::string> knownConstants;

        for (size_t i = 0; i < tokens.size(); ++i) {
            if (tokens[i].type == TokenType::Const &&
                i + 2 < tokens.size() &&
                tokens[i + 1].type == TokenType::Identifier &&
                tokens[i + 2].type == TokenType::Colon) {
                knownConstants.insert(tokens[i + 1].text);
            }
            else if (tokens[i].type == TokenType::Identifier &&
                i + 1 < tokens.size() &&
                tokens[i + 1].type == TokenType::Colon) {
                for (size_t j = i + 2; j < tokens.size(); ++j) {
                    if (tokens[j].type == TokenType::Const) {
                        knownConstants.insert(tokens[i].text);
                        break;
                    }
                    if (tokens[j].type == TokenType::Equal ||
                        tokens[j].type == TokenType::Semicolon ||
                        tokens[j].type == TokenType::Comma ||
                        tokens[j].type == TokenType::LBrace ||
                        tokens[j].type == TokenType::RBrace ||
                        tokens[j].type == TokenType::RParen) {
                        break;
                    }
                }
            }
        }

        for (size_t i = 0; i < tokens.size(); ++i) {
            const auto& token = tokens[i];
            int tokenModifier = 0;
            bool isClosingInactive = false;

            if (!inInactiveBlock && !waitingForInactiveBrace && token.type == TokenType::Identifier && i + 1 < tokens.size()) {
                int valIdx = -1;
                if (tokens[i + 1].type == TokenType::Equal && i + 2 < tokens.size()) {
                    valIdx = i + 2;
                }
                else if (i + 3 < tokens.size() && tokens[i + 1].type == TokenType::Colon && tokens[i + 3].type == TokenType::Equal && i + 4 < tokens.size()) {
                    valIdx = i + 4;
                }

                if (valIdx != -1) {
                    if (tokens[valIdx].type == TokenType::True) localComptimeVars[token.text] = true;
                    else if (tokens[valIdx].type == TokenType::False) localComptimeVars[token.text] = false;
                }
            }

            if (token.type == TokenType::At && i + 1 < tokens.size() && tokens[i + 1].text == "config") {
                nextIdentIsConfig = true;
            }
            if (nextIdentIsConfig && token.type == TokenType::Identifier) {
                configSymbols.insert(token.text);
                nextIdentIsConfig = false;
            }
            if (token.type == TokenType::Semicolon || token.type == TokenType::LBrace || token.type == TokenType::Equal) {
                nextIdentIsConfig = false;
            }

            if (token.type != TokenType::Else && token.type != TokenType::RBrace) {
                lastClosedBlockWasComptime = false;
            }

            if (token.type == TokenType::Struct && i + 1 < tokens.size() && tokens[i + 1].type == TokenType::Identifier) {
                activeStructForSemantics = tokens[i + 1].text;
            }

            if (token.type == TokenType::LBrace) {
                braceDepth++;
                isComptimeBlock[braceDepth] = nextBlockIsComptime;
                nextBlockIsComptime = false;

                if (!activeStructForSemantics.empty() && structBraceDepthForSemantics == -1) {
                    structBraceDepthForSemantics = braceDepth;
                }
                if (waitingForInactiveBrace) {
                    inactiveBraceDepth = braceDepth;
                    inInactiveBlock = true;
                    waitingForInactiveBrace = false;
                }
            }
            else if (token.type == TokenType::RBrace) {
                lastClosedBlockWasComptime = isComptimeBlock[braceDepth];
                if (structBraceDepthForSemantics == braceDepth) {
                    activeStructForSemantics = "";
                    structBraceDepthForSemantics = -1;
                }
                if (inInactiveBlock && inactiveBraceDepth == braceDepth) {
                    inInactiveBlock = false;
                    inactiveBraceDepth = -1;
                    isClosingInactive = true;
                }
                braceDepth--;
                if (!nsStack.empty() && nsStack.back().second == braceDepth) {
                    nsStack.pop_back();
                    currentNamespace.clear();
                    for (const auto& ns : nsStack) {
                        if (!currentNamespace.empty()) currentNamespace += "::";
                        currentNamespace += ns.first;
                    }
                }
            }
            else if (token.type == TokenType::Namespace && i + 1 < tokens.size() && tokens[i + 1].type == TokenType::Identifier) {
                std::string nsName = tokens[i + 1].text;
                nsStack.push_back({ nsName, braceDepth });
                if (currentNamespace.empty())
                    currentNamespace = nsName;
                else
                    currentNamespace += "::" + nsName;
            }

            if (!inInactiveBlock) {
                if (token.type == TokenType::Comptime && i + 1 < tokens.size() && tokens[i + 1].type == TokenType::If) {
                    size_t j = i + 2;
                    if (j < tokens.size() && tokens[j].type == TokenType::LParen) {
                        j++;
                        bool isNot = false;
                        if (j < tokens.size() && tokens[j].type == TokenType::Bang) { isNot = true; j++; }
                        if (j < tokens.size() && tokens[j].type == TokenType::Identifier) {
                            std::string varName = tokens[j].text;
                            bool val = localComptimeVars.count(varName) ? localComptimeVars[varName] : (Parser::m_comptimeVars.count(varName) ? Parser::m_comptimeVars[varName] != 0 : false);
                            bool condition = isNot ? !val : val;

                            comptimeChainTaken[braceDepth + 1] = condition;
                            waitingForInactiveBrace = !condition;
                            nextBlockIsComptime = true;
                        }
                    }
                }
                else if (token.type == TokenType::Else && lastClosedBlockWasComptime) {
                    if (i + 1 < tokens.size() && tokens[i + 1].type == TokenType::If) {
                        size_t j = i + 2;
                        if (j < tokens.size() && tokens[j].type == TokenType::LParen) {
                            j++;
                            bool isNot = false;
                            if (j < tokens.size() && tokens[j].type == TokenType::Bang) { isNot = true; j++; }
                            if (j < tokens.size() && tokens[j].type == TokenType::Identifier) {
                                std::string varName = tokens[j].text;
                                bool val = localComptimeVars.count(varName) ? localComptimeVars[varName] : (Parser::m_comptimeVars.count(varName) ? Parser::m_comptimeVars[varName] != 0 : false);
                                bool condition = isNot ? !val : val;

                                if (comptimeChainTaken.count(braceDepth + 1) && comptimeChainTaken[braceDepth + 1]) {
                                    waitingForInactiveBrace = true;
                                }
                                else {
                                    if (condition) {
                                        comptimeChainTaken[braceDepth + 1] = true;
                                        waitingForInactiveBrace = false;
                                    }
                                    else {
                                        waitingForInactiveBrace = true;
                                    }
                                }
                                nextBlockIsComptime = true;
                            }
                        }
                    }
                    else {
                        if (comptimeChainTaken.count(braceDepth + 1) && comptimeChainTaken[braceDepth + 1]) {
                            waitingForInactiveBrace = true;
                        }
                        else {
                            waitingForInactiveBrace = false;
                            comptimeChainTaken[braceDepth + 1] = true;
                        }
                        nextBlockIsComptime = true;
                    }
                }
            }

            int tokenTypeIdx = getSemanticTokenType(token.type, token.text, currentSema);

            if (!inMacro && token.type == TokenType::Identifier && token.text != "else") {
                bool isConfig = configSymbols.count(token.text) ||
                    configSymbols.count(currentNamespace.empty() ? token.text : currentNamespace + "::" + token.text);
                if (!isConfig && i >= 2 && tokens[i - 1].type == TokenType::DoubleColon && tokens[i - 2].type == TokenType::Identifier) {
                    if (configSymbols.count(tokens[i - 2].text + "_" + token.text)) isConfig = true;
                    if (configSymbols.count(tokens[i - 2].text + "::" + token.text)) isConfig = true;
                }

                if (isConfig) {
                    tokenTypeIdx = 0;
                }
                else if (token.text == "this" || token.text == "self") {
                    tokenTypeIdx = 0;
                }
                else if (knownConstants.count(token.text)) {
                    tokenTypeIdx = 4;
                    tokenModifier |= 4;
                }
                else if (knownGenericParams.count(token.text)) {
                    tokenTypeIdx = 1;
                }
                else {
                    int l = i;
                    while (l >= 2 && tokens[l - 1].type == TokenType::DoubleColon) l -= 2;
                    std::string fqn = "";
                    for (int k = l; k <= i; ++k) fqn += tokens[k].text;
                    std::string scopedFqn = currentNamespace.empty() ? fqn : currentNamespace + "::" + fqn;

                    if (i > 0 && tokens[i - 1].type == TokenType::Namespace) {
                        tokenTypeIdx = 11;
                    }
                    else if (i + 1 < tokens.size() && tokens[i + 1].type == TokenType::DoubleColon) {
                        if (currentSema && (currentSema->m_structs.count(token.text) ||
                            currentSema->m_generic_structs.count(token.text) ||
                            currentSema->m_enums.count(token.text) ||
                            currentSema->m_aliases.count(token.text))) {
                            tokenTypeIdx = 2;
                        }
                        else {
                            tokenTypeIdx = 11;
                        }
                    }
                    else if (i > 0 && tokens[i - 1].type == TokenType::Fn) {
                        bool isType = false;
                        int lookahead = i + 1;
                        if (lookahead < tokens.size() && tokens[lookahead].type == TokenType::LT) {
                            int depth = 1;
                            lookahead++;
                            while (lookahead < tokens.size() && depth > 0) {
                                if (tokens[lookahead].type == TokenType::LT) depth++;
                                else if (tokens[lookahead].type == TokenType::GT) depth--;
                                lookahead++;
                            }
                        }
                        if (lookahead < tokens.size() && tokens[lookahead].type == TokenType::DoubleColon) {
                            isType = true;
                        }
                        if (isType) {
                            tokenTypeIdx = 2;
                        }
                        else {
                            tokenTypeIdx = 3;
                        }
                    }
                    else if (i > 0 && (tokens[i - 1].type == TokenType::Struct || tokens[i - 1].type == TokenType::Enum || tokens[i - 1].type == TokenType::Alias)) {
                        tokenTypeIdx = 2;
                    }
                    else {
                        std::string methodFullName = "";
                        if (i >= 2 && tokens[i - 1].type == TokenType::DoubleColon) {
                            int baseIdx = i - 2;
                            if (baseIdx >= 0 && tokens[baseIdx].type == TokenType::GT) {
                                int depth = 1;
                                baseIdx--;
                                while (baseIdx >= 0 && depth > 0) {
                                    if (tokens[baseIdx].type == TokenType::GT) depth++;
                                    else if (tokens[baseIdx].type == TokenType::LT) depth--;
                                    baseIdx--;
                                }
                            }
                            std::string parentFqn = (baseIdx >= 0 && tokens[baseIdx].type == TokenType::Identifier) ? tokens[baseIdx].text : "";
                            if (currentSema && !parentFqn.empty() && (currentSema->m_structs.count(parentFqn) || currentSema->m_generic_structs.count(parentFqn))) {
                                methodFullName = parentFqn + "_" + token.text;
                            }
                        }

                        if (i + 1 < tokens.size() && tokens[i + 1].type == TokenType::LParen) {
                            tokenTypeIdx = 3;
                        }

                        if (currentSema) {
                            if (!methodFullName.empty() && (currentSema->m_functions.count(methodFullName) || currentSema->m_generic_functions.count(methodFullName))) tokenTypeIdx = 3;
                            else if (currentSema->m_functions.count(scopedFqn) || currentSema->m_functions.count(fqn) || currentSema->m_generic_functions.count(scopedFqn) || currentSema->m_generic_functions.count(fqn)) tokenTypeIdx = 3;
                            else if (currentSema->m_structs.count(scopedFqn) || currentSema->m_structs.count(fqn) ||
                                currentSema->m_generic_structs.count(scopedFqn) || currentSema->m_generic_structs.count(fqn)) tokenTypeIdx = 2;
                            else if (currentSema->m_enums.count(scopedFqn) || currentSema->m_enums.count(fqn)) tokenTypeIdx = 2;
                            else if (currentSema->m_aliases.count(scopedFqn) || currentSema->m_aliases.count(fqn)) tokenTypeIdx = 2;
                            else {
                                bool foundEnumMem = false;
                                for (const auto& [ename, enm] : currentSema->m_enums) {
                                    for (const auto& member : enm->members) {
                                        if (member.name == token.text) {
                                            tokenTypeIdx = 12;
                                            foundEnumMem = true;
                                            break;
                                        }
                                    }
                                    if (foundEnumMem) break;
                                }
                            }
                        }
                    }
                }
            }

            if (!inMacro && token.type == TokenType::LBracket && i + 1 < tokens.size() && tokens[i + 1].type == TokenType::LBracket) {
                inMacro = true;
            }
            if (inMacro) {
                tokenTypeIdx = 1;
            }
            if (inMacro && token.type == TokenType::RBracket && i > 0 && tokens[i - 1].type == TokenType::RBracket) {
                inMacro = false;
            }
            if (!inMacro) {
                if (token.type == TokenType::At) {
                    tokenTypeIdx = 8;
                    nextIsConstructorOffset = true;
                }
                else if (nextIsConstructorOffset && token.type == TokenType::IntLiteral) {
                    tokenTypeIdx = 8;
                    nextIsConstructorOffset = false;
                }
                else if (tokenTypeIdx != -1 && token.type != TokenType::At) {
                    nextIsConstructorOffset = false;
                }
            }

            if (inInactiveBlock || waitingForInactiveBrace || isClosingInactive) {
                tokenModifier |= 8;
            }

            if (tokenTypeIdx == -1) {
                if (inInactiveBlock || waitingForInactiveBrace || isClosingInactive) {
                    tokenTypeIdx = 6;
                }
                else {
                    continue;
                }
            }

            int line = token.loc.line - 1;
            int character = token.loc.col - 1;
            int length = token.text.length();
            int deltaLine = line - prevLine;
            int deltaChar = (deltaLine == 0) ? (character - prevChar) : character;

            data.push_back(deltaLine);
            data.push_back(deltaChar);
            data.push_back(length);
            data.push_back(tokenTypeIdx);
            data.push_back(tokenModifier);

            prevLine = line;
            prevChar = character;
        }

        sendResponse({
            {"jsonrpc", "2.0"},
            {"id", msg["id"]},
            {"result", {{"data", data}}}
        });
    }

    int LSPServer::getSemanticTokenType(TokenType type, const std::string& text, Sema* currentSema) {
        switch (type) {
            case TokenType::Fn: case TokenType::Return:
            case TokenType::Enum: case TokenType::Namespace:
            case TokenType::If: case TokenType::Else: case TokenType::While: case TokenType::Owner: case TokenType::For:
			case TokenType::Ref: case TokenType::Continue: case TokenType::Break:
                return 0;
            case TokenType::U8: case TokenType::U16: case TokenType::U32: case TokenType::U64:
            case TokenType::I8: case TokenType::I16: case TokenType::I32: case TokenType::I64:
            case TokenType::F32: case TokenType::F64: case TokenType::Void: case TokenType::Bool:
                return 1;
            case TokenType::Identifier:
                if (text == "else") return 0;

                if (currentSema) {
                    if (currentSema->m_structs.count(text)) return 2;
                    if (currentSema->m_generic_structs.count(text)) return 2;
                    if (currentSema->m_enums.count(text)) return 2;
                    if (currentSema->m_aliases.count(text)) return 2;
                    if (currentSema->m_functions.count(text)) return 3;

                    for (const auto& [ename, enm] : currentSema->m_enums) {
                        for (const auto& member : enm->members) {
                            if (member.name == text) return 12;
                        }
                    }
                }
                return 4;
            case TokenType::IntLiteral: case TokenType::FloatLiteral: return 5;
            case TokenType::Plus: case TokenType::Minus: case TokenType::Star: case TokenType::Slash:
            case TokenType::Equal: case TokenType::EqualEqual: case TokenType::NotEqual:
            case TokenType::LT: case TokenType::GT:
                return 6;
            case TokenType::LBrace: case TokenType::RBrace:
            case TokenType::LBracket: case TokenType::RBracket:
            case TokenType::LParen: case TokenType::RParen:
                return 6;
            case TokenType::At: case TokenType::Const:
            case TokenType::Cast: case TokenType::CastBits:
            case TokenType::Null: case TokenType::Lib:
            case TokenType::Struct: case TokenType::True:
            case TokenType::False: case TokenType::Volatile:
		    case TokenType::Sizeof: case TokenType::Variadic:
            case TokenType::Expand: case TokenType::Alignof:
		    case TokenType::Comptime: case TokenType::BuiltinAllocate:
            case TokenType::Lock: case TokenType::Operator:
                return 8;
            case TokenType::HashImport:
                return 8;
            case TokenType::Alias:
                return 8;
            case TokenType::StringLiteral:
                return 7;
            default: return -1;
        }
    }
}