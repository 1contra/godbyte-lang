/**
 * Copyright 2026 1contra
 *
 * Licensed under the Apache License, Version 2.0
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#include "../include/lexer.hpp"
#include "../include/parser.hpp"
#include "../include/sema.hpp"
#include "../include/irgen.hpp"
#include "../include/codegen.hpp"
#include "../utils/json.hpp"
#include "utils.hpp"
#include "config.hpp"

#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

using json = nlohmann::json;
using namespace gbpp;
namespace fs = std::filesystem;

struct ModuleDef {
    std::string sourceFile;
    std::unique_ptr<gbpp::Program> ast;
    bool isLib;
    std::string objPath;
};

int executeCommand(const std::string& cmd) {
#ifdef _WIN32
    std::string fullCmd = "cmd.exe /s /c \"" + cmd + "\"";
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessA(NULL, (LPSTR)fullCmd.c_str(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return exitCode;
#else
    pid_t pid = fork();
    if (pid == -1) return -1;
    if (pid == 0) { execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr); exit(127); }
    int status; waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

void loadImportsRecursively(const std::string& currentPath, std::vector<ModuleDef>& modules, std::set<std::string>& visited, bool isLib = false) {
    std::filesystem::path p(currentPath);
    if (!p.has_extension() || p.extension() != ".gbpp") p += ".gbpp";
    std::string absPath = fs::absolute(p).string();

    if (visited.count(absPath)) return;
    visited.insert(absPath);

    std::ifstream file(absPath);
    if (!file.is_open()) {
        divo::print_err_fatal("Could not open file: " + absPath);
        exit(1);
    }
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    gbpp::Lexer lexer(content, absPath);
    gbpp::Parser parser(lexer.tokenize());
    auto prog = parser.parse();
    if (parser.hasErrors) {
        divo::print_item_err("Syntax errors found in " + absPath + ":");
        for (const auto& err : parser.errors) {
            divo::print_item_err("  " + err);
        }
        exit(1);
    }
    if (!prog) exit(1);

    std::filesystem::path currentDir = p.parent_path();
    for (const auto& imp : prog->imports) {
        std::filesystem::path targetPath;
        bool impIsLib = imp->isLib;
        if (impIsLib) {
            targetPath = fs::current_path() / "libs" / (imp->path + ".gbpp");
            if (!fs::exists(targetPath)) {
                const char* envHome = std::getenv("DIVO_HOME");
                if (envHome) targetPath = fs::path(envHome) / "libs" / (imp->path + ".gbpp");
            }
        }
        else {
            targetPath = currentDir / imp->path;
        }
        loadImportsRecursively(targetPath.string(), modules, visited, impIsLib);
    }

    modules.push_back({ absPath, std::move(prog), isLib, "" });
}

void cmdAdd(int argc, char* argv[]) {
    if (argc < 3) {
        divo::print_err_fatal("Usage: divo add <library_name>");
        return;
    }

    std::string libName = argv[2];

    if (!fs::exists("divo.toml")) {
        divo::print_err_fatal("Could not find 'divo.toml'. Run 'divo init' first.");
        return;
    }

    if (divo::LibraryRegistry.find(libName) == divo::LibraryRegistry.end()) {
        divo::print_item_warn("Library '" + libName + "' is not a standard system library. Fetching AOT binary.");
    }

    if (!fs::exists("libs")) {
        fs::create_directory("libs");
    }

    const char* envHome = std::getenv("DIVO_HOME");
    if (!envHome) {
        divo::print_err_fatal("DIVO_HOME environment variable is not set. Please set it to your Divo installation path.");
        return;
    }

    fs::path globalLibSrcPath = fs::path(envHome) / "libs" / (libName + ".gbpp");
    fs::path localLibSrcPath = fs::path("libs") / (libName + ".gbpp");

#ifdef _WIN32
    std::string libBinName = libName + ".lib";
#else
    std::string libBinName = "lib" + libName + ".a";
#endif

    fs::path globalLibBinPath = fs::path(envHome) / "libs" / libBinName;
    fs::path localLibBinPath = fs::path("libs") / libBinName;

    if (!fs::exists(globalLibSrcPath)) {
        divo::print_err_fatal("Library interface '" + libName + ".gbpp' not found in DIVO_HOME/libs.");
        return;
    }

    divo::print_item("Copying " + libName + ".gbpp from DIVO_HOME...");
    try {
        fs::copy_file(globalLibSrcPath, localLibSrcPath, fs::copy_options::overwrite_existing);

        if (fs::exists(globalLibBinPath)) {
            divo::print_item("Copying pre-compiled binary " + libBinName + " from DIVO_HOME...");
            fs::copy_file(globalLibBinPath, localLibBinPath, fs::copy_options::overwrite_existing);
        }
        else if (divo::LibraryRegistry.find(libName) == divo::LibraryRegistry.end()) {
            divo::print_item_warn("Pre-compiled binary '" + libBinName + "' not found. Linkage may fail if not system provided.");
        }
    }
    catch (const fs::filesystem_error& e) {
        divo::print_err_fatal(std::string("Failed to copy library: ") + e.what());
        return;
    }

    if (divo::ConfigLoader::addDependencyToFile("divo.toml", libName)) {
        divo::print_header("Dependency Added");
        divo::print_footer("Added '" + libName + "' to project.", true);
    }
    else {
        divo::print_err_fatal("Failed to update divo.toml");
    }
}

void cmdInit() {
    if (fs::exists("divo.toml")) {
        divo::print_err_fatal("Config file 'divo.toml' already exists.");
        return;
    }
    divo::ConfigLoader::createDefault();

    if (!fs::exists("src")) {
        fs::create_directory("src");
        std::ofstream mainFile("src/main.gbpp");
        mainFile << "fn main(): u64 {\n    return 0;\n}\n";
    }
    if (!fs::exists("libs")) {
        fs::create_directory("libs");
        std::ofstream ioFile("libs/io.gbpp");
        ioFile << "// GodByte++ IO Library Interface\n";
    }

    divo::print_header("Initialization");
    divo::print_footer("Initialized new project in current directory", true);
}

struct SourceStats {
    int lines = 0;
    int tokens = 0;
    int functions = 0;
};

size_t computeSmartHash(const ModuleDef& mod, const std::string& cacheState, const std::vector<ModuleDef>& allModules) {
    size_t hash = divo::hashFile(mod.sourceFile) ^ std::hash<std::string>{}(cacheState);

    for (const auto& imp : mod.ast->imports) {
        hash ^= std::hash<std::string>{}(imp->path);
    }

    hash ^= std::hash<int>{}((int)mod.ast->functions.size());
    hash ^= std::hash<int>{}((int)mod.ast->structs.size());

    return hash;
}

void cmdBuild(int argc, char* argv[]) {
    divo::Config config = divo::ConfigLoader::load("divo.toml");
    if (!config.valid) {
        divo::print_err_fatal("Could not load 'divo.toml'. Run 'divo init'.");
        return;
    }

    std::string targetName = config.activeTarget;
    bool dumpIr = false;
    bool dumpAsm = false;
    bool enableOpt = false;
    bool tinyBuild = false;
    std::string customLinkArgs = "";

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.starts_with("--target=")) targetName = arg.substr(9);
        else if (arg == "--ir=true" || arg == "--ir") dumpIr = true;
        else if (arg == "--asm=true" || arg == "--asm") dumpAsm = true;
        else if (arg == "--opt=true" || arg == "-O") enableOpt = true;
        else if (arg == "--tiny") tinyBuild = true;
        else if (arg.starts_with("--link-args=")) customLinkArgs = arg.substr(12);
    }

    if (!config.targets.count(targetName)) {
        divo::print_err_fatal("Target '" + targetName + "' not defined in config.");
        return;
    }

    divo::TargetConfig target = config.targets[targetName];

    std::string profile = enableOpt ? "release" : "debug";
    std::string targetTriple = target.arch + "-" + target.os;
    std::string targetBase = "target/" + targetTriple + "/" + profile;

    std::string objDir = targetBase + "/deps";
    std::string logsDir = targetBase + "/logs";
    std::string cacheDir = targetBase + "/incremental";

    fs::create_directories(objDir);
    fs::create_directories(logsDir);
    fs::create_directories(cacheDir);

    divo::Logger::instance().init(logsDir + "/build.log");

    divo::print_header("Building " + config.projectName + " (Unity Build Target: " + targetName + ")");
    divo::Timer totalTime;
    SourceStats stats;

    json timingsArray = json::array();
    auto t_start = std::chrono::high_resolution_clock::now();
    auto recordTiming = [&](const std::string& phase, auto start_time) {
        auto end_time = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
        timingsArray.push_back({ {"phase", phase}, {"duration_ms", ms} });
        return end_time;
        };

    divo::print_group("Frontend Pipeline (File Aggregation)");

    std::vector<ModuleDef> modules;
    std::set<std::string> visited;

    loadImportsRecursively(config.entry, modules, visited, false);

    std::string cacheState = "ARGS:" + customLinkArgs + "|PROFILE:" + profile;
    size_t projectHash = std::hash<std::string>{}(cacheState);

    // Merge ASTs into a single Whole-Program AST
    gbpp::Program mergedProgram;

    for (auto& mod : modules) {
        projectHash ^= divo::hashFile(mod.sourceFile);
        stats.functions += (int)mod.ast->functions.size();

        // Standard library / Module merge logic
        for (auto& i : mod.ast->imports) mergedProgram.imports.push_back(std::move(i));
        for (auto& a : mod.ast->aliases) mergedProgram.aliases.push_back(std::move(a));
        for (auto& e : mod.ast->enums) mergedProgram.enums.push_back(std::move(e));
        for (auto& s : mod.ast->structs) mergedProgram.structs.push_back(std::move(s));
        for (auto& f : mod.ast->functions) mergedProgram.functions.push_back(std::move(f));
        for (auto& c : mod.ast->constants) mergedProgram.constants.push_back(std::move(c));
    }

    divo::print_item("Resolved and merged " + std::to_string(modules.size()) + " files into Unity AST.");
    auto t_frontend = recordTiming("Frontend Lex/Parse/Merge", t_start);

    divo::print_group("Middle-end Pipeline (Sema & IRGen)");

    std::vector<gbpp::Program*> progPtrs = { &mergedProgram };

    gbpp::Sema analyzer;
    if (!analyzer.analyzeModules(progPtrs)) {
        divo::print_item_err("Semantic Analysis Failed");
        return;
    }
    divo::print_item("Semantic analysis passed across unified program.");
    auto t_sema = recordTiming("Semantic Analysis", t_frontend);

    gbpp::Target backendTarget = gbpp::Target::Win64;
    if (target.arch == "esp32") backendTarget = gbpp::Target::SystemV;
    else if (target.os == "linux" || target.os == "macos") backendTarget = gbpp::Target::SystemV;

    std::string objExt = (target.os == "linux") ? ".o" : ".obj";
    std::string projectObjPath = objDir + "/" + config.projectName + objExt;
    std::string hashPath = cacheDir + "/project.hash";
    std::string targetExe = targetBase + "/" + fs::path(target.out).filename().string();

    bool needsLink = false;
    bool rebuild = true;

    if (fs::exists(hashPath) && fs::exists(projectObjPath)) {
        std::ifstream hf(hashPath);
        size_t oldHash = 0;
        if (hf >> oldHash && oldHash == projectHash) rebuild = false;
    }

    if (dumpIr && !fs::exists(targetBase + "/ir/" + config.projectName + ".ir")) rebuild = true;
    if (dumpAsm && !fs::exists(targetBase + "/asm/" + config.projectName + ".asm")) rebuild = true;

    if (rebuild) {
        needsLink = true;
        gbpp::IRGenerator irGen;
        auto irModule = irGen.generate(mergedProgram);
        irGen.optimize(irModule, enableOpt);

        if (dumpIr) {
            std::string irDir = targetBase + "/ir";
            fs::create_directories(irDir);
            std::string irPath = irDir + "/" + config.projectName + ".ir";
            std::ofstream irFile(irPath);
            irModule.print(irFile);
        }

        auto codegen = gbpp::CodeGen::create(backendTarget);

        if (dumpAsm) {
            std::string asmDir = targetBase + "/asm";
            fs::create_directories(asmDir);
            std::string asmPath = asmDir + "/" + config.projectName + ".asm";
            std::ofstream asmFile(asmPath);
            auto asmEmitter = gbpp::Emitter::createAsm();
            codegen->generate(irModule, *asmEmitter);
            asmEmitter->finalize(asmFile);
        }

        std::ofstream objFile(projectObjPath, std::ios::binary);
        std::unique_ptr<gbpp::Emitter> binEmitter = (backendTarget == gbpp::Target::Win64) ?
            gbpp::Emitter::createCoffWin64() : gbpp::Emitter::createElfSysV();

        codegen->generate(irModule, *binEmitter);
        binEmitter->finalize(objFile);
        objFile.close();

        std::ofstream hf(hashPath); hf << projectHash;
        divo::print_item("Compiled -> " + config.projectName + ".obj (Unity Build Optimized)");
    }

    if (!fs::exists(targetExe)) needsLink = true;
    auto t_codegen = recordTiming("IR & Code Generation", t_sema);

    if (!needsLink) {
        divo::print_group("Linking Phase");
        divo::print_item("Target artifacts are up to date (Skipped linker)");
    }
    else {
        divo::print_group("Linking Phase");

        if (target.arch == "x64") {
            std::string mapFile = targetBase + "/" + config.projectName + ".map";
            std::string pdbFile = targetBase + "/" + config.projectName + ".pdb";

            std::string dynamicLinkFlags = "";
            for (const auto& dep : config.dependencies) {
                if (divo::LibraryRegistry.count(dep)) {
                    auto& libInfo = divo::LibraryRegistry[dep];
                    dynamicLinkFlags += " " + ((target.os == "linux") ? libInfo.linkFlagsLin : libInfo.linkFlagsWin);
                }
                else {
                    dynamicLinkFlags += (target.os == "linux") ? (" libs/lib" + dep + ".a") : (" libs\\" + dep + ".lib");
                }
            }

            std::string linkCmd;
            if (target.os == "linux") {
                linkCmd = "gcc \"" + projectObjPath + "\" " + dynamicLinkFlags + " " + customLinkArgs + " -g -Wl,-Map=" + mapFile + " -no-pie -o " + targetExe;
            }
            else {
                if (!divo::setupMsvcEnvironment()) {
                    divo::print_item_warn("Could not reliably detect MSVC PATH. Ensure C++ Build Tools are installed.");
                }

                std::string debugFlags = enableOpt ? "" : " /DEBUG /PDB:" + pdbFile;
                std::string mapFlags = " /MAP:" + mapFile;

                if (tinyBuild) {
                    linkCmd = "link \"" + projectObjPath + "\" " + dynamicLinkFlags + " " + customLinkArgs + mapFlags + " /nologo /out:" + targetExe;
                }
                else {
                    linkCmd = "link \"" + projectObjPath + "\" " + dynamicLinkFlags + " " + customLinkArgs + mapFlags + debugFlags + " /subsystem:console /defaultlib:vcruntime /nologo /out:" + targetExe;
                }
            }

            divo::print_item("Linking executable -> " + targetExe);
            if (divo::runCmd(linkCmd) != 0) {
                divo::print_item_err("Linker failed.");
                return;
            }
        }
    }
    recordTiming("Linker", t_codegen);

    std::ofstream timingsFile(logsDir + "/timings.json");
    timingsFile << timingsArray.dump(4);

    divo::print_footer("Unity Build finished in " + std::to_string(totalTime.elapsed()) + "ms", true);
    divo::print_footer_sub("Output:  " + targetExe);
}

void printHelp(const std::string& command = "") {
    divo::print_header("GodByte++ Build System Help");

    if (command.empty()) {
        std::cout << "Usage: divo <command> [options]\n\n";
        std::cout << "Commands:\n";
        std::cout << "  init           Initialize a new project\n";
        std::cout << "  build          Build the project\n";
        std::cout << "  add <lib>      Add a library dependency\n";
        std::cout << "  clean          Remove build artifacts\n";
        std::cout << "  help [command] Show help for a command\n";
        std::cout << "\nUse 'divo help <command>' for details.\n";
    }
    else if (command == "init") {
        std::cout << "Usage: divo init\n";
        std::cout << "Initializes a new GodByte++ project in the current directory.\n";
    }
    else if (command == "build") {
        std::cout << "Usage: divo build [--target=<name>] [--ir] [--asm] [-O|--opt] [--tiny]\n";
        std::cout << "Builds the project for the specified target.\n";
        std::cout << "Options:\n";
        std::cout << "  --target=<name>  Specify the target to build (default: desktop)\n";
        std::cout << "  --ir             Dump the intermediate representation (IR)\n";
        std::cout << "  --asm            Dump the assembly output\n";
        std::cout << "  -O, --opt        Enable optimization passes\n";
        std::cout << "  --tiny           Build minimal executable (Windows only)\n";
    }
    else if (command == "add") {
        std::cout << "Usage: divo add <library_name>\n";
        std::cout << "Fetches a library interface and binary from the registry and adds it to your project.\n";
    }
    else if (command == "clean") {
        std::cout << "Usage: divo clean\n";
        std::cout << "Removes all build artifacts (build folder).\n";
    }
    else {
        std::cerr << "Unknown command for help: " << command << "\n";
    }

    divo::print_footer("GodByte++ Help", true);
}

int main(int argc, char* argv[]) {
    divo::enableVirtualTerminal();

    if (argc < 2) {
        printHelp();
        return 0;
    }

    std::string command = argv[1];
    if (command == "init") cmdInit();
    else if (command == "build") cmdBuild(argc, argv);
    else if (command == "add") cmdAdd(argc, argv);
    else if (command == "clean") {
        std::error_code ec;
        bool failed = false;

        if (fs::exists("target", ec)) {
            fs::remove_all("target", ec);
            if (ec) failed = true;
        }

        if (fs::exists("build", ec)) {
            fs::remove_all("build", ec);
            if (ec) failed = true;
        }

        divo::print_header("Clean");
        if (failed) {
            divo::print_item_err("Could not remove all artifacts (Files might be in use)");
            divo::print_footer("Clean completed with errors", false);
        }
        else {
            divo::print_footer("Removed target artifacts", true);
        }
    }
    else if (command == "help") {
        if (argc >= 3) printHelp(argv[2]);
        else printHelp();
    }
    else divo::print_err_fatal("Unknown command: " + command);

    return 0;
}