#pragma once
#include <string>
#include <fstream>
#include <vector>
#include <map>
#include <iostream>
#include "../utils/toml.hpp"

namespace divo {

    struct LibInfo {
        std::string name;
        std::string linkFlagsWin;
        std::string linkFlagsLin;
    };

    const char* sdk_env = std::getenv("VULKAN_SDK");
    std::string sdk = sdk_env ? sdk_env : "";

    static std::map<std::string, LibInfo> LibraryRegistry = {
        {"vulkan", {"vulkan", "/LIBPATH:\"" + sdk + "\\Lib\" vulkan-1.lib", "-lvulkan"}},
        {"io", {"io", "/defaultlib:msvcrt /defaultlib:ucrt", "-lc"}},
        {"math", {"math", "", "-lm"}}
    };

    struct TargetConfig {
        std::string name;
        std::string type = "executable";
        std::string arch = "x64";
        std::string os = "windows";
        std::string out = "game.exe";
        bool defined = false;
    };

    struct Config {
        std::string projectName = "Untitled";
        std::string version = "0.1.0";
        std::string entry = "src/main.gbpp";

        std::vector<std::string> dependencies;

        std::map<std::string, TargetConfig> targets;
        std::string activeTarget = "desktop";

        bool valid = false;
    };

    class ConfigLoader {
    public:
        static Config load(const std::string& path) {
            Config cfg;

            try {
                auto tbl = toml::parse_file(path);

                cfg.projectName = tbl["name"].value_or("Untitled");
                cfg.version = tbl["version"].value_or("0.1.0");
                cfg.entry = tbl["entry"].value_or("src/main.gbpp");

                if (auto deps = tbl["dependencies"].as_array()) {
                    for (auto& elem : *deps) {
                        if (auto depStr = elem.value<std::string>()) {
                            cfg.dependencies.push_back(*depStr);
                        }
                    }
                }

                if (auto targets = tbl["targets"].as_table()) {
                    for (auto& [k, v] : *targets) {
                        if (auto tTbl = v.as_table()) {
                            TargetConfig tc;
                            tc.name = std::string(k.str());
                            tc.type = tTbl->at_path("type").value_or("executable");
                            tc.arch = tTbl->at_path("arch").value_or("x64");
                            tc.os = tTbl->at_path("os").value_or("windows");
                            tc.out = tTbl->at_path("out").value_or("game.exe");
                            tc.defined = true;
                            cfg.targets[tc.name] = tc;
                        }
                    }
                }

                cfg.valid = true;
            }
            catch (const toml::parse_error& err) {
                std::cerr << "[Config Error] " << err.description() << " at line "
                    << err.source().begin.line << "\n";
                cfg.valid = false;
            }

            return cfg;
        }

        static void createDefault() {
            std::ofstream file("divo.toml");
            file << "name = \"main\"\n";
            file << "version = \"0.1.0\"\n";
            file << "entry = \"src/main.gbpp\"\n\n";

            file << "dependencies = [\"io\"]\n\n";

            file << "# Desktop Target (Default)\n";
            file << "[targets.desktop]\n";
            file << "type = \"executable\"\n";
            file << "arch = \"x64\"\n";
            file << "os = \"windows\"\n";
            file << "out = \"bin/main.exe\"\n";
            file.close();
        }

        static bool addDependencyToFile(const std::string& path, const std::string& libName) {
            try {
                auto tbl = toml::parse_file(path);

                if (auto deps = tbl["dependencies"].as_array()) {
                    for (auto& elem : *deps) {
                        if (elem.value<std::string>() == libName) {
                            return true;
                        }
                    }
                    deps->push_back(libName);
                }
                else {
                    auto newDeps = toml::array{};
                    newDeps.push_back(libName);
                    tbl.insert_or_assign("dependencies", newDeps);
                }

                std::ofstream file(path);
                file << tbl;
                return true;

            }
            catch (const std::exception& e) {
                std::cerr << "Failed to modify configuration: " << e.what() << "\n";
                return false;
            }
        }
    };
}