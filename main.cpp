/**
 * main.cpp — Auto-Doc Engine C++ API (Crow)
 * 
 * IMPLEMENTATION LOGIC:
 * 1. Isolated WorkDirs: Each request creates a unique folder to prevent race conditions.
 * 2. Robust IDs: Combines timestamp, PID, and a 64-bit random number.
 * 3. POSIX Standards: Decodes exit codes and signals (Segfaults, etc.) correctly.
 * 4. Sandbox: Enforces resource limits via 'timeout' and 'prlimit'.
 * 5. Manual CORS: Injects headers manually for reliable frontend connectivity.
 */

#include "crow.h"

struct CustomCORS {
    struct context {};
    void before_handle(crow::request&, crow::response&, context&) {}
    void after_handle(crow::request& req, crow::response& res, context&) {
        res.add_header("Access-Control-Allow-Origin", "*");
        res.add_header("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
        res.add_header("Access-Control-Allow-Headers", "Content-Type, Accept, Origin");
        if (req.method == crow::HTTPMethod::Options) {
            res.code = 200;
        }
    }
};

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

// ---------- Configuration ----------
static const std::string LOCKED_COMPILER_PATH = "g++";
static const std::string LOCKED_COMPILE_FLAGS = "-std=c++17 -O2 -Wall -Wextra";

// ---------- Helpers ----------

/**
 * Runs a shell command and returns stdout/stderr combined.
 * Captures the raw wait status for POSIX decoding.
 */
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

/**
 * Decodes POSIX wait status into a meaningful exit code or signal.
 */
static int decodeStatus(int status) {
#ifndef _WIN32
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
#endif
    return status;
}

/**
 * Generates a collision-resistant ID to ensure thread-safety and isolation.
 */
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

/**
 * Sanitizes filenames to prevent path traversal or shell injection.
 */
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

// ---------- API Logic ----------

int main() {
    crow::App<CustomCORS> app;

    // Base directory context
    const fs::path baseWorkDir = fs::temp_directory_path() / "auto_doc_engine";
    std::error_code ec;
    fs::create_directories(baseWorkDir, ec);

    // ---- Root Handler ----
    CROW_ROUTE(app, "/")
    ([]() {
        return crow::response(200, "Auto-Doc Engine Root OK");
    });

    // ---- Health Check ----
    CROW_ROUTE(app, "/api/health").methods("GET"_method, "OPTIONS"_method)
    ([](const crow::request& req) {
        if (req.method == crow::HTTPMethod::Options) { return crow::response(200); }

        crow::json::wvalue body;
        body["status"] = "ok";
        body["service"] = "Auto-Doc Engine C++ Production API";
        body["security"] = "Isolated WorkDirs + POSIX Decoding + Sandbox Active";
        return crow::response(200, body);
    });

    // ---- Compile + Execute ----
    CROW_ROUTE(app, "/api/compile").methods("POST"_method, "OPTIONS"_method)
    ([&baseWorkDir](const crow::request& req) {
        if (req.method == crow::HTTPMethod::Options) { return crow::response(200); }
        
        if (req.method != crow::HTTPMethod::Post) {
            return crow::response(405, "Method Not Allowed");
        }

        auto body = crow::json::load(req.body);
        if (!body) {
            return crow::response(400, "Invalid JSON");
        }

        // RULE: Each request gets a unique folder to prevent concurrent overwrites.
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

                // Write assets to unique directory
                {
                    std::ofstream ofs(sourcePath);
                    ofs << code;
                }
                if (!stdinData.empty()) {
                    std::ofstream ofs(stdinPath);
                    ofs << stdinData;
                }

                // Stage 1: Compilation
                int compileStatus = 0;
                std::string compileCmd = LOCKED_COMPILER_PATH + " " + LOCKED_COMPILE_FLAGS + " " + 
                                         quote(sourcePath.string()) + " -o " + quote(binaryPath.string());
                
                auto cStart = std::chrono::steady_clock::now();
                std::string compileOutput = runCommand(compileCmd, compileStatus);
                auto cEnd = std::chrono::steady_clock::now();
                
                r["compileOutput"] = compileOutput;
                r["compileTime"] = std::chrono::duration<double>(cEnd - cStart).count();

                if (decodeStatus(compileStatus) != 0) {
                    r["success"] = false;
                    r["error"] = "Compilation failed";
                    resultsArr.push_back(std::move(r));
                    continue;
                }

                // Stage 2: Sandbox Execution
                // RULE: Use timeout and prlimit for RAM (512MB), CPU (5s), and File limits (10MB).
                std::string runCmd;
#ifndef _WIN32
                runCmd = "timeout -k 1 5s prlimit --as=536870912 --nproc=1 --fsize=10485760 ";
#else
                runCmd = "";
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
                    else if (exitCode > 128) r["error"] = "Process crashed (Signal " + std::to_string(exitCode - 128) + ")";
                    else r["error"] = "Program exited with non-zero status";
                }

                resultsArr.push_back(std::move(r));
            }
        }

        // RULE: Self-contained cleanup to remove request artifacts.
        fs::remove_all(requestDir);

        crow::json::wvalue resBody;
        resBody["requestId"] = requestId;
        resBody["results"] = std::move(resultsArr);
        res.body = resBody.dump();
        return res;
    });

    // Railway Compatibility: Bind to dynamic $PORT or 18080 default
    const char* port_env = std::getenv("PORT");
    uint16_t port = port_env ? static_cast<uint16_t>(std::stoi(port_env)) : 18080;
    
    std::cout << "Auto-Doc Engine (Hardened) listening on 0.0.0.0:" << port << std::endl;
    app.bindaddr("0.0.0.0").port(port).multithreaded().run();
    return 0;
}