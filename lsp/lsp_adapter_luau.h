#pragma once
#include <memory>
#include <string>

class LSPAdapterLuau
{
public:
    LSPAdapterLuau();
    ~LSPAdapterLuau();

    bool initialize(const std::string& workspacePath);
    bool isInitialized() const { return initialized; }

    bool sendRequest(const std::string& request);
    std::string readResponse(int* contentLength = nullptr);

    std::string getLanguageId(const std::string& filePath) const;

private:
    class LuauImpl;
    std::unique_ptr<LuauImpl> impl;

    bool initialized = false;
};
