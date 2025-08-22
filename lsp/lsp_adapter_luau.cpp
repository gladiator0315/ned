#include "lsp_adapter_luau.h"
#include <windows.h>
#include <filesystem>
#include <vector>
#include <optional>
#include <iostream>
#include <nlohmann/json.hpp>
#include <algorithm>
#include "lsp_utils.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

// ── framing ───────────────────────────────────────────────────────────────────
static std::string makeFrame(const std::string& body){
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}
static bool writeAll(HANDLE h, const char* data, size_t len){
    size_t off=0; DWORD w=0;
    while(off<len){
        if(!WriteFile(h, data+off, (DWORD)std::min<size_t>(1<<15, len-off), &w, nullptr)) return false;
        off+=w;
    }
    return true;
}
static bool writeJson(HANDLE h, const std::string& json){ auto f=makeFrame(json); return writeAll(h,f.data(),f.size()); }

static std::optional<std::string> readFrame(HANDLE h){
    std::string header; header.reserve(128);
    char c; DWORD r=0;
    for(;;){
        DWORD avail=0; if(!PeekNamedPipe(h,nullptr,0,nullptr,&avail,nullptr)) return std::nullopt;
        if(avail==0){ Sleep(1); continue; }
        if(!ReadFile(h,&c,1,&r,nullptr) || r==0) return std::nullopt;
        header.push_back(c);
        if(header.size()>=4 && header.substr(header.size()-4)=="\r\n\r\n") break;
    }
    size_t pos = header.find("Content-Length:");
    if(pos==std::string::npos) return std::nullopt;
    size_t lineEnd = header.find("\r\n", pos);
    int len = std::stoi(header.substr(pos+15, lineEnd-(pos+15)));
    std::string body; body.resize((size_t)len);
    size_t got=0;
    while(got<(size_t)len){
        DWORD n=0;
        if(!ReadFile(h, &body[got], (DWORD)std::min<int>(1<<15, len-(int)got), &n, nullptr)) return std::nullopt;
        if(n==0){ Sleep(1); continue; }
        got+=n;
    }
    return body;
}

// ── process spawn ─────────────────────────────────────────────────────────────
struct Pipes { HANDLE hProcess=nullptr, inWr=nullptr, outRd=nullptr; };

static std::optional<Pipes> spawnLuau(const std::wstring& exe, const std::vector<std::wstring>& args){
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE outRd=0,outWr=0,inRd=0,inWr=0;
    if(!CreatePipe(&outRd,&outWr,&sa,0)) return std::nullopt;
    if(!SetHandleInformation(outRd, HANDLE_FLAG_INHERIT, 0)) return std::nullopt;
    if(!CreatePipe(&inRd,&inWr,&sa,0)) return std::nullopt;
    if(!SetHandleInformation(inWr, HANDLE_FLAG_INHERIT, 0)) return std::nullopt;

    STARTUPINFOW si{}; si.cb=sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput=inRd; si.hStdOutput=outWr; si.hStdError=outWr;

    std::wstring cmd=L"\"" + exe + L"\"";
    for(const auto& a:args) cmd+=L" \""+a+L"\"";

    PROCESS_INFORMATION pi{};
    if(!CreateProcessW(nullptr, cmd.data(), nullptr,nullptr, TRUE, CREATE_NO_WINDOW, nullptr,nullptr, &si, &pi)){
        CloseHandle(outWr); CloseHandle(inRd); CloseHandle(outRd); CloseHandle(inWr);
        return std::nullopt;
    }
    CloseHandle(outWr); CloseHandle(inRd);
    return Pipes{pi.hProcess, inWr, outRd};
}

// ── tiny helpers ──────────────────────────────────────────────────────────────
static fs::path exeDir(){
    wchar_t buf[MAX_PATH]; DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return n ? fs::path(buf).parent_path() : fs::current_path();
}

static fs::path bundledLuauExe(){
    auto p = exeDir() / "servers/luau-lsp/current/win-x64/luau-lsp.exe";
    return p;
}

// ── adapter impl ──────────────────────────────────────────────────────────────
class LSPAdapterLuau::LuauImpl {
public:
    ~LuauImpl(){ shutdown(); }

    bool start(const std::string& exeUtf8, const std::string& workspace){
        std::wstring exe(exeUtf8.begin(), exeUtf8.end());
        auto p = spawnLuau(exe, {L"lsp", L"--docs=./luau-config/en-us.json", L"--definitions=./luau-config/globalTypes.d.lua", L"--base-luaurc=./luau-config/.luaurc"});
        if(!p) return false;
        pipes = *p;

        // initialize (utf-8 positions + workspace)
        json init = {
            {"jsonrpc","2.0"},
            {"id",1},
            {"method","initialize"},
            {"params",{
                {"processId",(int)GetCurrentProcessId()},
                {"positionEncoding","utf-8"},
                {"rootUri", workspace.empty()? nullptr : json(pathToFileUri(workspace))},
                {"workspaceFolders", workspace.empty()? json::array() :
                    json::array({ {{"uri", pathToFileUri(workspace)},
                                   {"name", fs::path(workspace).filename().string()} } })},
                {"capabilities", {
                    {"textDocument", {
                        {"synchronization", {{"didSave", true}}},
                        {"completion", {{"completionItem", {{"snippetSupport", true}, {"documentationFormat", {"markdown","plaintext"}}}},
                                        {"contextSupport", true}}},
                        {"hover", {{"contentFormat", {"markdown","plaintext"}}}}
                    }}
                }}
            }}
        };

        if(!writeJson(pipes.inWr, init.dump())) return false;

        // wait for id:1 result then send "initialized"
        for(int i=0;i<40;i++){
            auto s = readFrame(pipes.outRd);
            if(!s || s->empty()) continue;
            if(s->find("\"id\":1")!=std::string::npos && s->find("\"result\"")!=std::string::npos){
                writeJson(pipes.inWr, R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
                return true;
            }
        }
        return false;
    }

    bool send(const std::string& req){ return writeJson(pipes.inWr, req); }
    std::string read(int *lenOut){
        auto s = readFrame(pipes.outRd);
        if(!s) return {};
        if(lenOut) *lenOut = (int)s->size();
        return *s;
    }

    void shutdown(){
        if(pipes.inWr){
            writeJson(pipes.inWr, R"({"jsonrpc":"2.0","id":9999,"method":"shutdown","params":{}})");
            writeJson(pipes.inWr, R"({"jsonrpc":"2.0","method":"exit","params":{}})");
            CloseHandle(pipes.inWr); pipes.inWr=nullptr;
        }
        if(pipes.outRd){ CloseHandle(pipes.outRd); pipes.outRd=nullptr; }
        if(pipes.hProcess){ WaitForSingleObject(pipes.hProcess, 50); CloseHandle(pipes.hProcess); pipes.hProcess=nullptr; }
    }

private:
    Pipes pipes{};
};

LSPAdapterLuau::LSPAdapterLuau() = default;
LSPAdapterLuau::~LSPAdapterLuau() = default;

bool LSPAdapterLuau::initialize(const std::string& workspacePath){
    if(initialized) return true;

    const fs::path exe = bundledLuauExe();
    if(!fs::exists(exe)){
        std::cerr << "[Luau] Missing bundled server at: " << exe.string()
                  << "\n       (expected servers/luau-lsp/current/win-x64/luau-lsp.exe next to your app)\n";
        return false;
    }

    impl = std::make_unique<LuauImpl>();
    if(!impl->start(exe.string(), workspacePath)){
        std::cerr << "[Luau] Failed to start: " << exe.string() << "\n";
        return false;
    }
    initialized = true;
    return true;
}

bool LSPAdapterLuau::sendRequest(const std::string& request){
    return impl ? impl->send(request) : false;
}

// std::string LSPAdapterLuau::readResponse(int *contentLength){
//     return impl ? impl->read(contentLength) : std::string();
// }

std::string LSPAdapterLuau::readResponse(int *contentLength) {
    if (contentLength) {
        *contentLength = -1;
    }

    if (!impl) {
        std::cerr << "[LSPAdapterLuau] readResponse: impl is null\n";
        return "";
    }

    try {
        std::string response = impl->read(contentLength);
        if (!response.empty()) {
            // Parse to check for errors
            try {
                json jsonResponse = json::parse(response);
                if (jsonResponse.contains("error")) {
                    std::cerr << "[LSPAdapterLuau] LSP Error: " << jsonResponse["error"].dump() << "\n";
                }
            } catch (...) {
                // Not JSON, that's fine
            }
            
            std::cout << "[LSPAdapterLuau] RX (" 
                      << (contentLength ? *contentLength : -1)
                      << "): " << response.substr(0, 120) << "...\n";
        }
        return response;
    } catch (const std::exception &e) {
        std::cerr << "[LSPAdapterLuau] Exception in readResponse: "
                  << e.what() << "\n";
        return "";
    }
}

std::string LSPAdapterLuau::getLanguageId(const std::string& /*filePath*/) const {
    return "luau";
}
