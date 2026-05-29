#pragma once
#include <iostream>
#include <string>
#include <unordered_map>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <chrono>
#include <atomic>
#include "../include/ast.hpp"
#include "../include/sema.hpp"
#include "../utils/json.hpp"

namespace gbpp::lsp {
    using json = nlohmann::json;

    class LSPServer {
    public:
        void run();
        ~LSPServer();

    private:
        std::unordered_map<std::string, std::string> documents;
        std::unordered_map<std::string, std::vector<Token>> importTokenCache;

        std::unique_ptr<Program> latestProgram;
        std::unique_ptr<Sema> latestSema;

        std::queue<json> messageQueue;
        std::mutex queueMutex;
        std::condition_variable queueCV;
        std::atomic<bool> isRunning{ false };
        std::thread workerThread;

        bool needsCompilation = false;
        std::chrono::steady_clock::time_point compileDeadline;
        std::string dirtyUri = "";

        void workerLoop();

        void handleMessage(const json& msg);
        void sendResponse(const json& response);
        void sendNotification(const std::string& method, const json& params);

        void handleInitialize(const json& msg);

        void handleDidOpen(const json& msg);
        void handleDidChange(const json& msg);

        void handleHover(const json& msg);
        void handleCompletion(const json& msg);
        void handleDefinition(const json& msg);
        void handleSemanticTokens(const json& msg);

        void compileAndPublishDiagnostics(const std::string& uri, const std::string& text);

        int getSemanticTokenType(TokenType type, const std::string& text, Sema* currentSema);
    };
}