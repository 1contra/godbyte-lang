#pragma once
#include <iostream>
#include <fstream>
#include <chrono>
#include <string>
#include <vector>
#include <filesystem>
#include <functional>
#include <cstdio>
#include <iomanip>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace divo {

    class Logger {
    public:
        static Logger& instance() { static Logger inst; return inst; }
        void init(const std::string& path) {
            if (logFile.is_open()) logFile.close();
            logFile.open(path);
        }

        void log(const std::string& msg) {
            if (logFile.is_open()) {
                auto now = std::chrono::system_clock::now();
                auto time_c = std::chrono::system_clock::to_time_t(now);
                logFile << "[" << std::put_time(std::localtime(&time_c), "%Y-%m-%d %H:%M:%S") << "] "
                    << msg << "\n";
                logFile.flush();
            }
        }

        void close() { if (logFile.is_open()) logFile.close(); }
        std::string getPath() const { return m_path; }
    private:
        std::ofstream logFile;
        std::string m_path;
        Logger() {}
    };

    inline void enableVirtualTerminal() {
#ifdef _WIN32
        SetConsoleOutputCP(CP_UTF8);
        system(" ");
#endif
    }

    inline int runCmd(const std::string & cmd) {
        Logger::instance().log("[EXEC]  " + cmd);
#ifdef _WIN32
            std::string fullCmd = "cmd.exe /s /c \"" + cmd + "\"";
            STARTUPINFOA si;
            PROCESS_INFORMATION pi;
            ZeroMemory(&si, sizeof(si));
            si.cb = sizeof(si);

            ZeroMemory(&pi, sizeof(pi));

            std::vector<char> cmdBuf(fullCmd.begin(), fullCmd.end());
            cmdBuf.push_back('\0');

            if (!CreateProcessA(NULL, cmdBuf.data(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
                return -1;
            }

            WaitForSingleObject(pi.hProcess, INFINITE);
            DWORD exitCode;
            GetExitCodeProcess(pi.hProcess, &exitCode);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            return exitCode;
#else
        pid_t pid = fork();
        if (pid == -1) return -1;
        if (pid == 0) { execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr); exit(127); }
        int status; waitpid(pid, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    }

    inline bool setupMsvcEnvironment() {
#ifdef _WIN32
        fs::create_directories("target");
        std::string cachePath = "target\\msvc_env.cache";

        auto loadEnv = [&](const std::string& path) {
            std::ifstream file(path);
            if (!file.is_open()) return false;

            std::string line;
            bool foundPath = false;
            while (std::getline(file, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();

                size_t eqPos = line.find('=');
                if (eqPos != std::string::npos) {
                    std::string name = line.substr(0, eqPos);
                    std::string value = line.substr(eqPos + 1);

                    std::string upperName = name;
                    for (char& c : upperName) c = toupper(c);

                    if (upperName == "PATH" || upperName == "INCLUDE" ||
                        upperName == "LIB" || upperName == "LIBPATH" || upperName == "VCINSTALLDIR") { 

                        SetEnvironmentVariableA(name.c_str(), value.c_str());
                        _putenv_s(name.c_str(), value.c_str());

                        if (upperName == "PATH") foundPath = true;
                    }
                }
            }
            return foundPath;
        };

        if (fs::exists(cachePath)) {
            loadEnv(cachePath);
        }

        if (std::getenv("VCINSTALLDIR") != nullptr) {
            return true;
        }

        auto checkLink = []() {
            return divo::runCmd("link /? 2>&1 | findstr /i \"Microsoft\" > NUL") == 0;
        };
        if (checkLink()) return true;

        std::string vcvarsPath = "";
        const char* progFiles86 = std::getenv("ProgramFiles(x86)");
        if (progFiles86) {
            std::string vswherePath = std::string(progFiles86) + "\\Microsoft Visual Studio\\Installer\\vswhere.exe";
            if (fs::exists(vswherePath)) {
                std::string vswhereCmd = "\"\"" + vswherePath + "\" -latest -products * -property installationPath 2> NUL\"";
                FILE* pipe = _popen(vswhereCmd.c_str(), "r");
                if (pipe) {
                    char buffer[512];
                    std::vector<std::string> vsPaths;
                    while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                        std::string line = buffer;
                        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
                        if (!line.empty()) vsPaths.push_back(line);
                    }
                    _pclose(pipe);

                    for (const auto& p : vsPaths) {
                        std::string testPath = p + "\\VC\\Auxiliary\\Build\\vcvars64.bat";
                        if (fs::exists(testPath)) {
                            vcvarsPath = testPath;
                            break;
                        }
                    }
                }
            }
        }

        if (vcvarsPath.empty()) {
            const char* fallbacks[] = {
                "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat",
                "C:\\Program Files\\Microsoft Visual Studio\\2022\\Professional\\VC\\Auxiliary\\Build\\vcvars64.bat",
                "C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise\\VC\\Auxiliary\\Build\\vcvars64.bat",
                "C:\\Program Files\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Auxiliary\\Build\\vcvars64.bat",
                "C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat",
                "C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\BuildTools\\VC\\Auxiliary\\Build\\vcvars64.bat"
            };
            for (auto& path : fallbacks) {
                if (fs::exists(path)) { vcvarsPath = path; break; }
            }
        }

        if (vcvarsPath.empty()) return false;

        std::string cmd = "call \"" + vcvarsPath + "\" < NUL > NUL 2> NUL && set > \"" + fs::absolute(cachePath).string() + "\"";
        divo::runCmd(cmd);

        return loadEnv(cachePath) && checkLink();
#else
        return true;
#endif
    }

    inline size_t hashFile(const std::string& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return 0;
        std::ostringstream ss;
        ss << file.rdbuf();
        return std::hash<std::string>{}(ss.str());
    }

    namespace Color {
        const std::string RESET = "\x1b[0m";
        const std::string GREEN = "\x1b[32m";
        const std::string YELLOW = "\x1b[33m";
        const std::string RED = "\x1b[31m";
        const std::string CYAN = "\x1b[36m";
        const std::string GRAY = "\x1b[90m";
        const std::string BOLD_GREEN = "\x1b[1;32m";
        const std::string BOLD_CYAN = "\x1b[1;36m";
        const std::string BOLD_WHITE = "\x1b[1;37m";
        const std::string BOLD_RED = "\x1b[1;31m";
    }

    namespace Trace {
        const std::string START = "\xe2\x94\x8c";
        const std::string BAR = "\xe2\x94\x82";
        const std::string SPLIT = "\xe2\x94\x9c";
        const std::string MID = "\xe2\x94\x80";
        const std::string END = "\xe2\x94\x94";
        const std::string ARROW = "\xe2\x9e\x9c";
    }

    class Timer {
        using Clock = std::chrono::high_resolution_clock;
        std::chrono::time_point<Clock> start;
    public:
        Timer() : start(Clock::now()) {}
        double elapsed() {
            auto now = Clock::now();
            return std::chrono::duration<double, std::milli>(now - start).count();
        }
    };

    inline void print_header(const std::string& message) {
        std::cout << Color::BOLD_CYAN << Trace::START << Trace::MID << Trace::MID << " "
            << Color::BOLD_WHITE << message << Color::RESET << "\n";
    }

    inline void print_group(const std::string& message) {
        std::cout << Color::BOLD_CYAN << Trace::BAR << "\n"
            << Trace::SPLIT << Trace::MID << " "
            << Color::BOLD_WHITE << message << Color::RESET << "\n";
    }

    inline void print_item(const std::string& message) {
        std::cout << Color::BOLD_CYAN << Trace::BAR << "  "
            << Trace::SPLIT << Trace::MID << " "
            << Color::RESET << message << "\n";
    }

    inline void print_item_cmd(const std::string& cmd) {
        std::cout << Color::BOLD_CYAN << Trace::BAR << "  "
            << Trace::BAR << "  " << Trace::END << Trace::MID << " "
            << Color::GRAY << "running: " << cmd << Color::RESET << "\n";
    }

    inline void print_item_warn(const std::string& message) {
        std::cout << Color::BOLD_CYAN << Trace::BAR << "  "
            << Color::YELLOW << Trace::END << Trace::MID << " "
            << Trace::ARROW << " " << message << Color::RESET << "\n";
    }

    inline void print_item_err(const std::string& s) {
        std::cerr << Color::BOLD_CYAN << Trace::BAR << "  "
            << Color::RED << Trace::END << Trace::MID << " "
            << Trace::ARROW << " ERROR: " << s << Color::RESET << "\n";
    }

    inline void print_err_fatal(const std::string& s) {
        std::cerr << Color::BOLD_RED << "[FATAL] " << s << Color::RESET << "\n";
    }

    inline void print_footer(const std::string& message, bool success = true) {
        std::cout << (success ? Color::BOLD_GREEN : Color::BOLD_RED)
            << Trace::END << Trace::MID << " "
            << message << Color::RESET << "\n";
    }

    inline void print_footer_sub(const std::string& message) {
        std::cout << "   " << Trace::END << Trace::MID << " "
            << Color::GRAY << message << Color::RESET << "\n";
    }
}

class InterfaceGenerator {
public:
    static void generate(gbpp::Program* prog, const std::string& outPath) {
        std::ofstream out(outPath);
        out << "// GodByte++ Auto-Generated Interface File\n";
        out << "// Target: Library Front-End (AOT Linkage)\n\n";

        for (auto& imp : prog->imports) {
            out << "#import ";
            if (imp->isLib) out << "lib ";
            out << "\"" << imp->path << "\";\n";
        }
        if (!prog->imports.empty()) out << "\n";

        auto getNs = [](const std::string& fullName) {
            size_t pos = fullName.rfind("::");
            return pos != std::string::npos ? fullName.substr(0, pos) : "";
        };
        auto getName = [](const std::string& fullName) {
            size_t pos = fullName.rfind("::");
            return pos != std::string::npos ? fullName.substr(pos + 2) : fullName;
        };

        auto hasExport = [](const std::set<std::string>& attrs) {
            for (const auto& a : attrs) {
                if (a == "export" || a == "@export") return true;
            }
            return false;
        };

        std::map<std::string, std::vector<gbpp::EnumDecl*>> enumsByNs;
        std::map<std::string, std::vector<gbpp::StructDecl*>> structsByNs;
        std::map<std::string, std::vector<gbpp::FunctionDecl*>> funcsByNs;

        for (auto& e : prog->enums) {
            if (hasExport(e->attributes)) enumsByNs[getNs(e->name)].push_back(e.get());
        }
        for (auto& s : prog->structs) {
            if (hasExport(s->attributes)) structsByNs[getNs(s->name)].push_back(s.get());
        }
        for (auto& f : prog->functions) {
            if (hasExport(f->attributes)) {
                funcsByNs[getNs(f->name)].push_back(f.get());
            }
            else {
                std::string fns = getNs(f->name);
                std::string fname = getName(f->name);
                bool belongsToExportedStruct = false;
                for (auto& s : prog->structs) {
                    if (hasExport(s->attributes) && getNs(s->name) == fns) {
                        if (fname.starts_with(getName(s->name) + "_")) {
                            belongsToExportedStruct = true;
                            break;
                        }
                    }
                }
                if (belongsToExportedStruct) {
                    funcsByNs[fns].push_back(f.get());
                }
            }
        }

        std::set<std::string> namespaces;
        for (auto& kv : enumsByNs) namespaces.insert(kv.first);
        for (auto& kv : structsByNs) namespaces.insert(kv.first);
        for (auto& kv : funcsByNs) namespaces.insert(kv.first);

        for (const auto& ns : namespaces) {
            std::vector<std::string> nsParts;
            size_t start = 0, end = 0;
            while ((end = ns.find("::", start)) != std::string::npos) {
                nsParts.push_back(ns.substr(start, end - start));
                start = end + 2;
            }
            if (start < ns.length() && !ns.empty()) nsParts.push_back(ns.substr(start));

            std::string indent = "";
            for (const auto& p : nsParts) {
                out << indent << "namespace " << p << " {\n";
                indent += "    ";
            }

            for (auto e : enumsByNs[ns]) {
                out << indent << "[[@extern]]\n";
                out << indent << "enum " << getName(e->name) << " {\n";
                for (auto& m : e->members) {
                    out << indent << "    " << m.name << " = " << m.value << ",\n";
                }
                out << indent << "};\n\n";
            }

            for (auto s : structsByNs[ns]) {
                out << indent << "[[@extern]]\n";
                out << indent << "struct " << getName(s->name) << " {\n";
                for (auto& f : s->fields) {
                    out << indent << "    " << f.name << ": " << f.parsedType.toString() << ";\n";
                }
                out << indent << "};\n\n";
            }

            for (auto f : funcsByNs[ns]) {
                out << indent << "[[@extern]]\n";
                out << indent << "fn " << getName(f->name) << "(";
                for (size_t i = 0; i < f->params.size(); ++i) {
                    out << f->params[i].name << ": " << f->params[i].parsedType.toString();
                    if (i + 1 < f->params.size()) out << ", ";
                }
                out << "): " << f->returnType.toString() << ";\n\n";
            }

            for (int i = (int)nsParts.size() - 1; i >= 0; --i) {
                indent = indent.substr(0, indent.length() - 4);
                out << indent << "}\n";
            }
            if (!nsParts.empty()) out << "\n";
        }
        out.close();
    }
};