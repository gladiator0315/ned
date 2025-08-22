#include "lsp.h"
#include "../editor/editor.h"
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include "lsp_utils.h"

EditorLSP::EditorLSP() : currentRequestId(1000) {}

EditorLSP::~EditorLSP() = default;

bool EditorLSP::initialize(const std::string &workspacePath)
{
	std::cout << "\033[35mLSP:\033[0m Initializing with workspace path: " << workspacePath
			  << std::endl;

	// Delegate to the LSP manager
	return gLSPManager.initialize(workspacePath);
}

std::string EditorLSP::escapeJSON(const std::string &s) const
{
	std::string out;
	out.reserve(s.length() * 2);
	for (char c : s)
	{
		switch (c)
		{
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\b':
			out += "\\b";
			break;
		case '\f':
			out += "\\f";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if ('\x00' <= c && c <= '\x1f')
			{
				char buf[8];
				snprintf(buf, sizeof buf, "\\u%04x", c);
				out += buf;
			} else
			{
				out += c;
			}
		}
	}
	return out;
}

void EditorLSP::didOpen(const std::string &filePath, const std::string &content)
{
	// Select the appropriate adapter for this file
	if (!gLSPManager.selectAdapterForFile(filePath))
	{
		return;
	}

	// If the selected adapter is not initialized, initialize it with the
	// current file's directory
	if (!gLSPManager.isInitialized())
	{
		// Extract workspace path from file path (use directory containing the
		// file)
		std::string workspacePath = filePath.substr(0, filePath.find_last_of("/\\"));
		std::cout << "\033[35mLSP:\033[0m Auto-initializing with workspace: "
				  << workspacePath << std::endl;

		try
		{
			if (!gLSPManager.initialize(workspacePath))
			{
				std::cout << "\033[31mLSP:\033[0m Failed to initialize LSP for "
						  << filePath << std::endl;
				return;
			}
		} catch (const std::exception &e)
		{
			std::cout << "\033[31mLSP:\033[0m Exception during LSP initialization: "
					  << e.what() << std::endl;
			std::cout
				<< "\033[33mLSP:\033[0m LSP support will be disabled for this session"
				<< std::endl;
			return;
		} catch (...)
		{
			std::cout << "\033[31mLSP:\033[0m Unknown exception during LSP initialization"
					  << std::endl;
			std::cout
				<< "\033[33mLSP:\033[0m LSP support will be disabled for this session"
				<< std::endl;
			return;
		}
	}

	const std::string uri = pathToFileUri(filePath);
    std::string notification = std::string(R"({
            "jsonrpc": "2.0",
            "method": "textDocument/didOpen",
            "params": {
                "textDocument": {
                    "uri": ")") + uri + R"(",
                    "languageId": ")" +
                               gLSPManager.getLanguageId(filePath) + R"(",
                    "version": 1,
                    "text": ")" +
                               escapeJSON(content) +
                               R"("
                }
            }
        })";

	// Only send request if we have a working adapter
	if (gLSPManager.hasWorkingAdapter())
	{
		gLSPManager.sendRequest(notification);
		// std::cout << "\033[32mLSP:\033[0m didOpen notification sent successfully"
		// << std::endl;
	} else
	{
		std::cout
			<< "\033[33mLSP:\033[0m Skipping LSP request - no working adapter available"
			<< std::endl;
	}
}

void EditorLSP::didChange(const std::string &filePath, int version)
{
    static std::chrono::steady_clock::time_point lastChangeTime;
    static std::string lastChangeContent;
    
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastChangeTime).count();

    if (elapsed < 50 && editor_state.fileContent == lastChangeContent) {
        return;
    }
    
    lastChangeTime = now;
    lastChangeContent = editor_state.fileContent;
    
    std::cout << "[LSP] didChange -> " << filePath 
              << " v" << version << " len=" << editor_state.fileContent.size() << "\n";
	
	// Select the appropriate adapter for this file
	if (!gLSPManager.selectAdapterForFile(filePath))
	{
		return;
	}

	// Auto-initialize if needed
	if (!gLSPManager.isInitialized())
	{
		std::string workspacePath = filePath.substr(0, filePath.find_last_of("/\\"));
		std::cout << "\033[35mLSP:\033[0m Auto-initializing with workspace: "
				  << workspacePath << std::endl;

		try
		{
			if (!gLSPManager.initialize(workspacePath))
			{
				std::cout << "\033[31mLSP:\033[0m Failed to initialize LSP for "
						  << filePath << std::endl;
				return;
			}
		} catch (const std::exception &e)
		{
			std::cout << "\033[31mLSP:\033[0m Exception during LSP initialization: "
					  << e.what() << std::endl;
			std::cout
				<< "\033[33mLSP:\033[0m LSP support will be disabled for this session"
				<< std::endl;
			return;
		} catch (...)
		{
			std::cout << "\033[31mLSP:\033[0m Unknown exception during LSP initialization"
					  << std::endl;
			std::cout
				<< "\033[33mLSP:\033[0m LSP support will be disabled for this session"
				<< std::endl;
			return;
		}
	}

	const std::string uri = pathToFileUri(filePath);
	std::string notification = std::string(R"({
	"jsonrpc":"2.0","method":"textDocument/didChange","params":{
		"textDocument":{"uri":")") + uri + R"(","version":)" + std::to_string(version) + R"(},
		"contentChanges":[{"text":")" + escapeJSON(editor_state.fileContent) + R"("}]
	}})";

	// Only send request if we have a working adapter
	if (gLSPManager.hasWorkingAdapter())
	{
		if (gLSPManager.sendRequest(notification))
		{
			// std::cout << "\033[32mLSP:\033[0m didChange notification sent
			// successfully (v" << version
			// << ")\n";
		} else
		{
			// std::cout << "\033[31mLSP:\033[0m Failed to send didChange
			// notification\n";
		}
	} else
	{
		std::cout
			<< "\033[33mLSP:\033[0m Skipping LSP request - no working adapter available"
			<< std::endl;
	}
}

void EditorLSP::didSave(const std::string& filePath, int /*version*/) {
    if (!gLSPManager.selectAdapterForFile(filePath)) return;
    if (!gLSPManager.isInitialized()) {
        std::string ws = filePath.substr(0, filePath.find_last_of("/\\"));
        try { if (!gLSPManager.initialize(ws)) return; } catch (...) { return; }
    }
    const std::string uri = pathToFileUri(filePath);
	std::string notification = std::string(R"({
	"jsonrpc":"2.0","method":"textDocument/didSave","params":{
		"textDocument":{"uri":")") + uri + R"("},
		"text":")" + escapeJSON(editor_state.fileContent) + R"("
	}})";
    if (gLSPManager.hasWorkingAdapter()) gLSPManager.sendRequest(notification);
}

void EditorLSP::didClose(const std::string& filePath) {
    if (!gLSPManager.selectAdapterForFile(filePath)) return;
    if (!gLSPManager.isInitialized()) {
        std::string ws = filePath.substr(0, filePath.find_last_of("/\\"));
        try { if (!gLSPManager.initialize(ws)) return; } catch (...) { return; }
    }
    const std::string uri = pathToFileUri(filePath);
	std::string notification = std::string(R"({
	"jsonrpc":"2.0","method":"textDocument/didClose","params":{
		"textDocument":{"uri":")") + uri + R"("}
	}})";
    if (gLSPManager.hasWorkingAdapter()) gLSPManager.sendRequest(notification);
}