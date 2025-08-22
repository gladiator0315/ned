// lsp_autocomplete.h
#pragma once

#include "../editor/editor.h"
#include "../editor/editor_cursor.h"
#include "../lib/json.hpp"
#include "imgui.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

struct CompletionRequest
{
	std::string filePath;
	int line;
	int character;
	int requestId;
};

struct CompletionDisplayItem
{
	std::string label;
	std::string detail;
	std::string insertText;
	std::string sortText; // Added for LSP server's sorting
	int kind;
	int startLine = -1;
	int startChar = -1;
	int endLine = -1;
	int endChar = -1;
	float score = 0.0f; // Add score field for sorting
};

class LSPAutocomplete
{
  public:
	LSPAutocomplete();
	~LSPAutocomplete();

	void requestCompletion(const std::string &filePath, int line, int character);
	void renderCompletions();

	bool showCompletions = false;
	int selectedCompletionIndex = 0;
	ImVec2 completionPopupPos = ImVec2(0, 0);

	bool blockTab = false;
	bool blockEnter = false;

	// State tracking for focus/frame logic
	static bool wasShowingLastFrame;

	public:
		size_t getCompletionCount() const { return currentCompletionItems.size(); }

  private:
	std::vector<CompletionDisplayItem> currentCompletionItems;
	std::queue<CompletionRequest> requestQueue;
	std::mutex queueMutex;
	std::condition_variable queueCondition;
	std::atomic<bool> shouldStop{false};
	std::thread workerThread;

	// Track active requests to preserve original coordinates
	std::unordered_map<int, CompletionRequest> activeRequests;
	std::mutex activeRequestsMutex;

	// Position caching for smooth menu updates
	ImVec2 lastPopupPos;
	std::chrono::steady_clock::time_point lastPositionUpdate;
	static constexpr int POSITION_CACHE_DURATION_MS = 2000; // 2 seconds

	// --- Private Helper Functions for Rendering ---
	bool shouldRender();
	bool handleInputAndCheckClose(); // Returns true if the window should close
	void calculateWindowGeometry(ImVec2 &outWindowSize, ImVec2 &outSafePos);
	void applyStyling();
	void renderCompletionListItems();
	bool handleClickOutside();
	void finalizeRenderState();
	void resetPopupPosition(); // New function to reset position cache

	// requesting logic
	std::string cleanSnippetFormatting(const std::string& text);
	std::string formCompletionRequest(int requestId,
									  const std::string &filePath,
									  int line,
									  int character);
	bool processResponse(const std::string &response, int requestId);
	void parseCompletionResult(const json &result, int requestLine, int requestCharacter);
	void updatePopupPosition();
	void workerFunction(); // New method for background thread

	void
	insertText(int row_start, int col__start, int row_end, int col__end, std::string text);

	std::pair<int, int> getLineAndCharFromIndex(int index) const
	{
		if (editor_state.editor_content_lines.empty() || index < 0)
			return {0, 0};

		// Find the line containing this index
		int line = 0;
		for (; line < static_cast<int>(editor_state.editor_content_lines.size()); ++line)
		{
			if (editor_state.editor_content_lines[line] > index)
			{
				break;
			}
		}
		line = std::max(0, line - 1);

		// Calculate character position within line
		int line_start = editor_state.editor_content_lines[line];
		return {line, index - line_start};
	}

	
	private:
		enum class CompletionContext {
			Global,
			PropertyAccess,    // object.
			FunctionCall,      // function(
			TableAccess,       // table[
			StringMethod,      // string:
			Unknown
		};

	private:
		// Context-aware filtering and prioritization
		bool shouldIncludeCompletion(const CompletionDisplayItem& item, CompletionContext context);
		void prioritizeCompletions(std::vector<CompletionDisplayItem>& items, 
								CompletionContext context,
								const std::string& currentWord);
		void insertCompletion(const CompletionDisplayItem& item);
		
		// Enhanced UI rendering
		void renderCompletionItem(const CompletionDisplayItem& item, bool isSelected);
		void renderDocumentationTooltip(const CompletionDisplayItem& item);
		
		// Debouncing and performance
		std::chrono::steady_clock::time_point lastCompletionRequestTime;
		static constexpr int COMPLETION_DEBOUNCE_MS = 150;
		
		// Snippet handling
		int cursorOffset = 0;
    
    CompletionContext detectCompletionContext(int cursorPos) {
        if (cursorPos <= 0) return CompletionContext::Global;
        
        std::string content = editor_state.fileContent;
        
        // Look backwards from cursor to determine context
        for (int i = cursorPos - 1; i >= 0; i--) {
            char c = content[i];
            
            if (c == ':') return CompletionContext::PropertyAccess;
            if (c == ':') return CompletionContext::StringMethod;
            if (c == '[') return CompletionContext::TableAccess;
            if (c == '(') return CompletionContext::FunctionCall;
            
            // Skip whitespace and comments
            if (!isspace(c) && c != '-' && c != '-') {
                break;
            }
        }
        
        return CompletionContext::Global;
    }
};
