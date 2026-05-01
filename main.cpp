// main.cpp — Auto-Doc Engine C++ API (Crow)
// Receives .cpp source code over JSON, compiles it with g++, executes the
// resulting binary, captures stdout/stderr, and returns structured JSON.

#include "crow.h"
#include "crow/middlewares/cors.h"

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

static std::string runCommand(const std::string& cmd, int& exitCode) {
    std::array<char, 4096> buffer{};
    std::string output;

    std::string fullCmd = cmd + " 2>&1";

    FILE* pipe = PIPE_OPEN(fullCmd.c_str(), "r");
    if (!pipe) {
        exitCode = -1;
        return "Failed to start subprocess.";
    }
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        output.append(buffer.data());
    }
    int status = PIPE_CLOSE(pipe);
    exitCode = status;
    return output;
}

static std::string generateUniqueId() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<int> dist(0, 15);
    static const char hexChars[] = "0123456789abcdef";

    std::string id;
    id.reserve(16);
    for (int i = 0; i < 16; ++i) id.push_back(hexChars[dist(gen)]);
    return id;
}

static std::string sanitizeFilename(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if (std::isalnum(static_cast<unsigned char>(c)) ||
            c == '_' || c == '-' || c == '.') {
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

static std::string toShellPath(const fs::path& p) {
    return p.generic_string();
}

static void removeStaleBuildArtifacts(const fs::path& root) {
    std::error_code ec;
    const fs::path outDir  = root / "out";
    const fs::path tempDir = root / "temp";
    if (fs::exists(outDir,  ec)) fs::remove_all(outDir,  ec);
    if (fs::exists(tempDir, ec)) fs::remove_all(tempDir, ec);

    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec) break;
        if (entry.is_regular_file(ec) && entry.path().extension() == ".o") {
            fs::remove(entry.path(), ec);
        }
    }
}

// ---------- main ----------

int main() {
    crow::App<crow::CORSHandler> app;

    // ---- CORS ----
    // FIX: use exact origin instead of wildcard so Authorization header
    // and future credentialed requests work correctly in all browsers.
    auto& cors = app.get_middleware<crow::CORSHandler>();
    cors.global()
        .headers("Content-Type", "Authorization", "Accept")
        .methods("POST"_method, "GET"_method, "OPTIONS"_method)
        .origin("http://localhost:5173");

    // Working directory for source files and binaries.
    fs::path workDir = fs::temp_directory_path() / "auto_doc_engine";
    std::error_code ec;
    fs::create_directories(workDir, ec);
    if (ec) {
        std::cerr << "[FATAL] Cannot create work directory: "
                  << workDir << " — " << ec.message() << std::endl;
        return 1;
    }

    // ---- Health check ----
    CROW_ROUTE(app, "/api/health").methods("GET"_method, "OPTIONS"_method)
    ([](const crow::request& req) {
        // FIX: handle OPTIONS preflight on every route
        if (req.method == crow::HTTPMethod::Options) {
            return crow::response(204);
        }
        crow::json::wvalue res;
        res["status"]       = "ok";
        res["service"]      = "Auto-Doc Engine C++ API";
        res["compiler"]     = LOCKED_COMPILER_PATH;
        res["compileFlags"] = LOCKED_COMPILE_FLAGS;
        return crow::response(200, res);
    });

    // ---- Compile + run ----
    // FIX: added OPTIONS"_method so the browser preflight gets a 204
    // instead of a 404 — this was the root cause of the CORS block.
    CROW_ROUTE(app, "/api/compile").methods("POST"_method, "OPTIONS"_method)
    ([&workDir](const crow::request& req) {

        // FIX: short-circuit OPTIONS preflight immediately.
        // Crow's CORSHandler will attach the Access-Control-* headers.
        if (req.method == crow::HTTPMethod::Options) {
            return crow::response(204);
        }

        auto body = crow::json::load(req.body);
        if (!body) {
            crow::json::wvalue err;
            err["error"] = "Invalid JSON body.";
            return crow::response(400, err);
        }
        if (!body.has("files") || body["files"].size() == 0) {
            crow::json::wvalue err;
            err["error"] = "No files provided in 'files' array.";
            return crow::response(400, err);
        }

        const std::string stdinData =
            body.has("input") ? std::string(body["input"].s()) : std::string{};

        removeStaleBuildArtifacts(workDir);

        std::vector<crow::json::wvalue> resultsArr;
        const size_t fileCount = body["files"].size();
        resultsArr.reserve(fileCount);

        for (size_t i = 0; i < fileCount; ++i) {
            const auto& entry = body["files"][i];

            std::string filename =
                entry.has("filename") ? std::string(entry["filename"].s())
                                      : "file.cpp";
            std::string code =
                entry.has("code") ? std::string(entry["code"].s())
                                  : std::string{};
            filename = sanitizeFilename(filename);

            crow::json::wvalue r;
            r["filename"]   = filename;
            r["sourceCode"] = code;

            if (code.empty()) {
                r["success"]       = false;
                r["stage"]         = "input";
                r["error"]         = "Empty source code.";
                r["compileOutput"] = "";
                r["runOutput"]     = "";
                resultsArr.push_back(std::move(r));
                continue;
            }

            const std::string id         = generateUniqueId();
            const fs::path    sourcePath = workDir / (id + ".cpp");
            const fs::path    binaryPath = workDir / (id + EXE_EXT);
            const fs::path    stdinPath  = workDir / (id + ".in");

            // Write source file.
            {
                std::ofstream ofs(sourcePath, std::ios::binary);
                if (!ofs) {
                    r["success"]       = false;
                    r["stage"]         = "io";
                    r["error"]         = "Cannot write source file to disk.";
                    r["compileOutput"] = "";
                    r["runOutput"]     = "";
                    resultsArr.push_back(std::move(r));
                    continue;
                }
                ofs << code;
            }

            // Optional stdin.
            if (!stdinData.empty()) {
                std::ofstream ofs(stdinPath, std::ios::binary);
                ofs << stdinData;
            }

            // Compile.
            const std::string compileCmd =
                LOCKED_COMPILER_PATH + " " + LOCKED_COMPILE_FLAGS + " " +
                quote(toShellPath(sourcePath)) + " -o " +
                quote(toShellPath(binaryPath));

            const auto compileStart = std::chrono::steady_clock::now();
            int compileExit = 0;
            std::string compileOutput = runCommand(compileCmd, compileExit);
            const auto compileEnd = std::chrono::steady_clock::now();
            const double compileTime =
                std::chrono::duration<double>(compileEnd - compileStart).count();

            r["compileOutput"] = compileOutput;
            r["compileTime"]   = compileTime;

            const bool compiled = (compileExit == 0) && fs::exists(binaryPath);
            if (!compiled) {
                r["success"]   = false;
                r["stage"]     = "compile";
                r["error"]     = "Compilation failed.";
                r["runOutput"] = "";
                std::error_code rmEc;
                fs::remove(sourcePath, rmEc);
                if (fs::exists(stdinPath)) fs::remove(stdinPath, rmEc);
                resultsArr.push_back(std::move(r));
                continue;
            }

            // Execute (5-second wall-clock limit on POSIX via timeout).
            std::string runCmd;
        #ifdef _WIN32
            runCmd = quote(toShellPath(binaryPath));
        #else
            runCmd = "timeout 5s " + quote(binaryPath.string());
        #endif
            if (!stdinData.empty()) {
                runCmd += " < " + quote(toShellPath(stdinPath));
            }

            const auto runStart = std::chrono::steady_clock::now();
            int runExit = 0;
            std::string runOutput = runCommand(runCmd, runExit);
            const auto runEnd = std::chrono::steady_clock::now();
            const double runTime =
                std::chrono::duration<double>(runEnd - runStart).count();

            r["runOutput"]     = runOutput;
            r["executionTime"] = runTime;
            r["exitCode"]      = runExit;
            r["success"]       = (runExit == 0);
            r["stage"]         = "run";
            if (runExit != 0) {
                r["error"] = (runExit == 124 || runExit == 31744)
                    ? "Execution exceeded 5 second time limit."
                    : "Program exited with non-zero status.";
            }

            // Cleanup.
            std::error_code rmEc;
            fs::remove(sourcePath, rmEc);
            fs::remove(binaryPath, rmEc);
            if (fs::exists(stdinPath)) fs::remove(stdinPath, rmEc);

            resultsArr.push_back(std::move(r));
        }

        crow::json::wvalue response;
        response["totalFiles"] = static_cast<int>(fileCount);
        response["results"]    = std::move(resultsArr);
        return crow::response(200, response);
    });

    // ---- FIX: read Railway's dynamic $PORT instead of hardcoding 18080 ----
    // Railway assigns a random port at container startup via the $PORT env var.
    // Hardcoding 18080 means Railway's router can't reach your process,
    // causing every request to fail even though the URL resolves correctly.
    const char* port_env = std::getenv("PORT");
    uint16_t port = port_env ? static_cast<uint16_t>(std::stoi(port_env)) : 18080;

    std::cout << "===========================================" << std::endl;
    std::cout << " Auto-Doc Engine — C++ API (Crow)"            << std::endl;
    std::cout << " Listening on   : http://0.0.0.0:" << port    << std::endl;
    std::cout << " Compile route  : POST /api/compile"          << std::endl;
    std::cout << " Health route   : GET  /api/health"           << std::endl;
    std::cout << "===========================================" << std::endl;

    app.bindaddr("0.0.0.0").port(port).multithreaded().run();
    return 0;
}