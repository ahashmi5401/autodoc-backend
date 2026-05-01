/**
 * main.cpp — Auto-Doc Engine C++ API (Crow)
 * Compliance: Isolated WorkDirs, POSIX decoding, prlimit, robust IDs.
 */

#include "crow.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

#ifdef _WIN32
    #define PIPE_OPEN  _popen
    #define PIPE_CLOSE _pclose
    static const std::string EXE_EXT = ".exe";
#else
    #define PIPE_OPEN  popen
    #define PIPE_CLOSE pclose
    static const std::string EXE_EXT = "";
#endif

static const std::string LOCKED_COMPILER_PATH = "g++";
static const std::string LOCKED_COMPILE_FLAGS = "-std=c++17 -O2 -Wall -Wextra";

// ---------- helpers ----------

static std::string runCommand(const std::string& cmd, int& waitStatus) {
    std::array<char, 4096> buffer{};
    std::string output;
    std::string fullCmd = cmd + " 2>&1";

    FILE* pipe = PIPE_OPEN(fullCmd.c_str(), "r");
    if (!pipe) {
        waitStatus = -1;
        return "Failed to start subprocess.";
    }
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        output.append(buffer.data());
    }
    waitStatus = PIPE_CLOSE(pipe);
    return output;
}

static int decodeStatus(int status) {
#ifndef _WIN32
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
#endif
    return status;
}

static std::string generateRobustId() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;

    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    uint64_t rnd = dist(gen);
    
    std::stringstream ss;
    ss << std::hex << now << "_" << getpid() << "_" << rnd;
    return ss.str();
}

static std::string sanitizeFilename(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    if (out.empty()) out = "file.cpp";
    return out;
}

static std::string quote(const std::string& s) {
    return "\"" + s + "\"";
}

int main() {
    crow::SimpleApp app;

    // Base work dir config
    const fs::path baseWorkDir = fs::temp_directory_path() / "auto_doc_engine";
    std::error_code ec;
    fs::create_directories(baseWorkDir, ec);

    // ---- Health check ----
    CROW_ROUTE(app, "/api/health").methods("GET"_method, "OPTIONS"_method)
    ([](const crow::request& req) {
        crow::response res;
        res.add_header("Access-Control-Allow-Origin", "*");
        if (req.method == crow::HTTPMethod::Options) { res.code = 204; return res; }

        crow::json::wvalue body;
        body["status"] = "ok";
        body["security"] = "Hardened (Isolated WorkDirs + Resource Limits)";
        res.code = 200;
        res.body = body.dump();
        return res;
    });

    // ---- Compile + run ----
    CROW_ROUTE(app, "/api/compile").methods("POST"_method, "OPTIONS"_method)
    ([&baseWorkDir](const crow::request& req) {
        crow::response res;
        res.add_header("Access-Control-Allow-Origin", "*");
        if (req.method == crow::HTTPMethod::Options) { res.code = 204; return res; }

        auto body = crow::json::load(req.body);
        if (!body) return crow::response(400, "Invalid JSON");

        // CRITICAL FIX: Every request gets its OWN unique subdirectory
        std::string requestId = generateRobustId();
        fs::path requestDir = baseWorkDir / requestId;
        fs::create_directories(requestDir);

        const std::string stdinData = body.has("input") ? std::string(body["input"].s()) : "";
        std::vector<crow::json::wvalue> resultsArr;

        if (body.has("files")) {
            for (const auto& entry : body["files"]) {
                std::string filename = sanitizeFilename(entry.has("filename") ? std::string(entry["filename"].s()) : "main.cpp");
                std::string code = entry.has("code") ? std::string(entry["code"].s()) : "";

                crow::json::wvalue r;
                r["filename"] = filename;

                fs::path sourcePath = requestDir / filename;
                fs::path binaryPath = requestDir / (filename + ".bin");
                fs::path stdinPath  = requestDir / "stdin.txt";

                {
                    std::ofstream ofs(sourcePath);
                    ofs << code;
                }
                if (!stdinData.empty()) { std::ofstream ofs(stdinPath); ofs << stdinData; }

                // Compile
                int compileStatus = 0;
                std::string compileOutput = runCommand("g++ -std=c++17 -O2 -Wall -Wextra " + quote(sourcePath.string()) + " -o " + quote(binaryPath.string()), compileStatus);
                
                r["compileOutput"] = compileOutput;
                if (decodeStatus(compileStatus) != 0) {
                    r["success"] = false;
                    r["error"] = "Compilation failed";
                    resultsArr.push_back(std::move(r));
                    continue;
                }

                // Run with Sandbox Limits
                // FIX: Added timeout and prlimit (Linux)
                std::string runCmd;
#ifndef _WIN32
                runCmd = "timeout -k 1 5s prlimit --as=536870912 --nproc=1 --fsize=10485760 ";
#endif
                runCmd += quote(binaryPath.string());
                if (!stdinData.empty()) runCmd += " < " + quote(stdinPath.string());

                int runStatus = 0;
                auto start = std::chrono::steady_clock::now();
                std::string runOutput = runCommand(runCmd, runStatus);
                auto end = std::chrono::steady_clock::now();

                int exitCode = decodeStatus(runStatus);
                r["runOutput"] = runOutput;
                r["exitCode"] = exitCode;
                r["executionTime"] = std::chrono::duration<double>(end - start).count();
                r["success"] = (exitCode == 0);

                if (exitCode != 0) {
                    if (exitCode == 124) r["error"] = "Time limit exceeded (5s)";
                    else if (exitCode > 128) r["error"] = "Program crashed (Signal " + std::to_string(exitCode - 128) + ")";
                    else r["error"] = "Non-zero exit code";
                }

                resultsArr.push_back(std::move(r));
            }
        }

        // Cleanup this specific request's folder
        fs::remove_all(requestDir);

        crow::json::wvalue response;
        response["results"] = std::move(resultsArr);
        res.body = response.dump();
        return res;
    });

    const char* port_env = std::getenv("PORT");
    uint16_t port = port_env ? static_cast<uint16_t>(std::stoi(port_env)) : 18080;
    app.bindaddr("0.0.0.0").port(port).multithreaded().run();
    return 0;
}
