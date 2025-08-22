// lsp_autocomplete.cpp
#include "lsp_autocomplete.h"
#include "../editor/editor.h"
#include "../editor/editor_cursor.h"
#include "../editor/editor_tree_sitter.h"
#include "lib/json.hpp"
#include "lsp.h"
#include "lsp_manager.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "lsp_globals.h"
#include "lsp_utils.h"


using json = nlohmann::json;

bool LSPAutocomplete::wasShowingLastFrame = false;

LSPAutocomplete::LSPAutocomplete()
{
	// Start the worker thread
	workerThread = std::thread(&LSPAutocomplete::workerFunction, this);
}

LSPAutocomplete::~LSPAutocomplete()
{
	// Signal the worker thread to stop
	shouldStop = true;
	queueCondition.notify_one();

	// Wait for the thread to finish
	if (workerThread.joinable())
	{
		workerThread.join();
	}
}

void LSPAutocomplete::workerFunction()
{
	while (!shouldStop)
	{
		CompletionRequest request;
		{
			std::unique_lock<std::mutex> lock(queueMutex);
			queueCondition.wait(lock,
								[this] { return !requestQueue.empty() || shouldStop; });

			if (shouldStop)
			{
				break;
			}

			request = requestQueue.front();
			requestQueue.pop();
		}

		// Process the request
		if (!gLSPManager.isInitialized())
		{
			std::cout << "\033[31mLSP Autocomplete:\033[0m Not initialized" << std::endl;
			continue;
		}

		if (!gLSPManager.selectAdapterForFile(request.filePath))
		{
			std::cout << "\033[31mLSP Autocomplete:\033[0m No LSP adapter "
						 "available for file: "
					  << request.filePath << std::endl;
			continue;
		}

		std::cout << "\033[35mLSP Autocomplete:\033[0m Processing request at line "
				  << request.line << ", char " << request.character
				  << " (ID: " << request.requestId << ")" << std::endl;

		// Store the request for later coordinate retrieval
		{
			std::lock_guard<std::mutex> lock(activeRequestsMutex);
			activeRequests[request.requestId] = request;
		}

		// Form and send request
		std::string request_str = formCompletionRequest(
			request.requestId, request.filePath, request.line, request.character);
		if (!gLSPManager.sendRequest(request_str))
		{
			std::cout << "\033[31mLSP Autocomplete:\033[0m Failed to send request"
					  << std::endl;
			// Remove from active requests if send failed
			std::lock_guard<std::mutex> lock(activeRequestsMutex);
			activeRequests.erase(request.requestId);
			continue;
		}

		// Wait for response with timeout
		const int MAX_ATTEMPTS = 15;
		const int WAIT_MS = 50;
		for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(WAIT_MS));
			int contentLength = 0;
			std::string response = gLSPManager.readResponse(&contentLength);

			if (!response.empty())
			{
				if (processResponse(response, request.requestId))
				{
					break;
				}
			}
		}
	}
}

void LSPAutocomplete::requestCompletion(const std::string &filePath,
										int line,
										int character)
{
	int requestId = gEditorLSP.getNextRequestId();

	{
		std::lock_guard<std::mutex> lock(queueMutex);
		requestQueue.push({filePath, line, character, requestId});
	}
	queueCondition.notify_one();
}

bool LSPAutocomplete::shouldRender()
{
	if (!showCompletions || currentCompletionItems.empty())
	{
		// remvoed due to turning off blocking every frame...
		/*
		if (editor_state.block_input) {
			editor_state.block_input = false;
		}
		*/
		wasShowingLastFrame = false;
		return false;
	}
	return true;
}

bool LSPAutocomplete::handleInputAndCheckClose()
{
	bool closeAndUnblock = false;
	bool navigationKeyPressed = false;

	// Close completion menu for Delete, Backspace, or Space
	if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace) ||
		ImGui::IsKeyPressed(ImGuiKey_Space))
	{
		closeAndUnblock = true;
		resetPopupPosition(); // Reset position on close
	}

	if (ImGui::IsKeyPressed(ImGuiKey_Escape))
	{
		std::cout << "[renderCompletions] Escape pressed, hiding completions."
				  << std::endl;
		closeAndUnblock = true;
		resetPopupPosition(); // Reset position on escape
	} else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
	{
		if (!currentCompletionItems.empty())
		{
			if (selectedCompletionIndex > 0)
			{
				selectedCompletionIndex--;
			}
		}
		navigationKeyPressed = true;
		editor_state.block_input = true; // Only block input during navigation
		resetPopupPosition();			 // Reset position on navigation
	} else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
	{
		if (!currentCompletionItems.empty())
		{
			if (selectedCompletionIndex < currentCompletionItems.size() - 1)
			{
				selectedCompletionIndex++;
			}
		}
		navigationKeyPressed = true;
		editor_state.block_input = true; // Only block input during navigation
		resetPopupPosition();			 // Reset position on navigation
	} else if (ImGui::IsKeyPressed(ImGuiKey_Enter) ||
			   ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
	{
		closeAndUnblock = true;
		resetPopupPosition(); // Reset position on enter
	} else if (ImGui::IsKeyPressed(ImGuiKey_Tab))
	{
		if (selectedCompletionIndex >= 0 &&
			selectedCompletionIndex < currentCompletionItems.size())
		{
			blockTab = true;
			const auto &selected_item = currentCompletionItems[selectedCompletionIndex];
			insertText(selected_item.startLine,
					   selected_item.startChar,
					   selected_item.endLine,
					   selected_item.endChar,
					   selected_item.insertText);
		}
		closeAndUnblock = true;
	}

	if (!closeAndUnblock && !navigationKeyPressed)
	{
		ImGuiIO &io = ImGui::GetIO();
		if (io.InputQueueCharacters.Size > 0)
		{
			// Check if any of the input characters should close the menu
			bool shouldClose = false;
			for (int n = 0; n < io.InputQueueCharacters.Size; n++)
			{
				char c = static_cast<char>(io.InputQueueCharacters[n]);
				if (c == '.' || c == '(' || c == ')' || c == '[' || c == ']' ||
					c == '{' || c == '}' || c == ',' || c == ';' || c == ':' ||
					c == '+' || c == '-' || c == '*' || c == '/' || c == '=' ||
					c == '!' || c == '&' || c == '|' || c == '^' || c == '%' ||
					c == '<' || c == '>')
				{
					shouldClose = true;
					break;
				}
			}
			if (shouldClose)
			{
				closeAndUnblock = true;
			}
		} else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
		{
			std::cout << "[renderCompletions] Left Arrow pressed, hiding completions."
					  << std::endl;
			closeAndUnblock = true;
		} else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
		{
			std::cout << "[renderCompletions] Right Arrow pressed, hiding "
						 "completions."
					  << std::endl;
			closeAndUnblock = true;
		}
	}

	// --- Process Closing Action ---
	if (closeAndUnblock)
	{
		editor_state.block_input = false;
		showCompletions = false;
		wasShowingLastFrame = false;
		return true;
	}

	// If we're not navigating and not closing, ensure input is not blocked
	if (!navigationKeyPressed)
	{
		editor_state.block_input = false;
	}

	return false;
}

void LSPAutocomplete::insertText(
	int row_start, int col__start, int row_end, int col__end, std::string text)
{
	int start_index, end_index;

	// Bounds check for row indices
	if (row_start < 0 || row_start >= editor_state.editor_content_lines.size() ||
		row_end < 0 || row_end >= editor_state.editor_content_lines.size())
	{
		std::cerr << "Invalid row positions - using cursor insertion" << std::endl;
		start_index = end_index = editor_state.cursor_index;
	} else
	{
		start_index = editor_state.editor_content_lines[row_start] + col__start;
		end_index = editor_state.editor_content_lines[row_end] + col__end;

		// Additional bounds checking
		if (start_index < 0 || end_index < 0 || start_index > end_index ||
			start_index > editor_state.fileContent.size() ||
			end_index > editor_state.fileContent.size())
		{
			std::cerr << "Invalid positions - using cursor insertion" << std::endl;
			start_index = end_index = editor_state.cursor_index;
		}
	}
	// Debug output removed for performance

	// Delete existing content in both buffers
	if (start_index <= end_index && end_index <= editor_state.fileContent.size())
	{
		// Erase from fileContent
		editor_state.fileContent.erase(start_index, end_index - start_index);

		// Erase from fileColors
		auto colors_begin = editor_state.fileColors.begin() + start_index;
		auto colors_end = editor_state.fileColors.begin() + end_index;
		editor_state.fileColors.erase(colors_begin, colors_end);
	}

	// Insert new text and colors
	if (!text.empty())
	{
		// Insert into fileContent
		editor_state.fileContent.insert(start_index, text);

		// Get the proper default text color from the theme
		TreeSitter::updateThemeColors();
		ImVec4 defaultColor = TreeSitter::cachedColors.text;

		// Optionally extend the previous character's color for better visual
		// continuity
		ImVec4 insertColor = defaultColor;
		if (start_index > 0 && start_index <= editor_state.fileColors.size())
		{
			// Use the color of the character before the insertion point
			insertColor = editor_state.fileColors[start_index - 1];
		}

		editor_state.fileColors.insert(editor_state.fileColors.begin() + start_index,
									   text.size(),
									   insertColor);
	}
	editor_state.cursor_index = start_index + text.size();
	editor_state.text_changed = true;
	gEditor.updateLineStarts();
	gEditorHighlight.highlightContent();
}

void LSPAutocomplete::calculateWindowGeometry(ImVec2 &outWindowSize, ImVec2 &outSafePos)
{
	const int current_item_count = currentCompletionItems.size();
	const float item_height = ImGui::GetTextLineHeightWithSpacing();
	const float window_padding = 5.0f;
	const float desired_width = 300.0f;
	const float max_visible_items = 10.0f;

	float current_list_height =
		std::min((float)current_item_count, max_visible_items) * item_height;
	outWindowSize = ImVec2(desired_width, current_list_height + window_padding * 2.0f);

	ImVec2 calculated_popup_anchor_pos = completionPopupPos;

	ImGuiViewport *viewport = ImGui::GetMainViewport();
	outSafePos = calculated_popup_anchor_pos;
	float editor_char_height = editor_state.line_height;
	outSafePos.y += editor_char_height;

	if ((outSafePos.y + outWindowSize.y) > (viewport->Pos.y + viewport->Size.y - 5.0f))
	{
		outSafePos.y = calculated_popup_anchor_pos.y - outWindowSize.y - 2.0f;
	}
	if ((outSafePos.x + outWindowSize.x) > (viewport->Pos.x + viewport->Size.x - 5.0f))
	{
		outSafePos.x = calculated_popup_anchor_pos.x - outWindowSize.x;
	}
	if (outSafePos.x < (viewport->Pos.x + 5.0f))
	{
		outSafePos.x = viewport->Pos.x + 5.0f;
	}
	if (outSafePos.y < (viewport->Pos.y + 5.0f))
	{
		outSafePos.y = viewport->Pos.y + 5.0f;
	}
}

void LSPAutocomplete::applyStyling()
{
	const float window_padding = 5.0f;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
						ImVec2(window_padding, window_padding));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
	ImGui::PushStyleColor(
		ImGuiCol_WindowBg,
		ImVec4(gSettings.getSettings()["backgroundColor"][0].get<float>() * .8,
			   gSettings.getSettings()["backgroundColor"][1].get<float>() * .8,
			   gSettings.getSettings()["backgroundColor"][2].get<float>() * .8,
			   1.0f));
	ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(1.0f, 0.1f, 0.7f, 0.4f));
}

void LSPAutocomplete::renderCompletionItem(const CompletionDisplayItem& item, 
                                          bool isSelected) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float lineHeight = ImGui::GetTextLineHeight();
    
    // Draw background for selected item
    if (isSelected) {
        ImU32 selectionColor = ImGui::GetColorU32(ImGuiCol_Header);
        drawList->AddRectFilled(pos, ImVec2(pos.x + ImGui::GetContentRegionAvail().x, 
                                          pos.y + lineHeight), selectionColor);
    }
    
    // Draw icon based on completion kind
    const char* icon = "?";
    ImU32 iconColor = IM_COL32(200, 200, 200, 255);
    
    switch (item.kind) {
        case 1: icon = "T"; iconColor = IM_COL32(86, 156, 214, 255); break; // Text
        case 2: icon = "ƒ"; iconColor = IM_COL32(220, 220, 170, 255); break; // Function
        case 3: icon = "C"; iconColor = IM_COL32(78, 201, 176, 255); break; // Constructor
        case 4: icon = "F"; iconColor = IM_COL32(184, 215, 163, 255); break; // Field
        case 5: icon = "V"; iconColor = IM_COL32(156, 220, 254, 255); break; // Variable
        case 6: icon = "c"; iconColor = IM_COL32(197, 134, 192, 255); break; // Class
        case 7: icon = "I"; iconColor = IM_COL32(86, 156, 214, 255); break; // Interface
        default: icon = "?"; break;
    }
    
    // Draw icon
    ImVec2 iconPos(pos.x + 5, pos.y + (lineHeight - ImGui::GetTextLineHeight()) * 0.5f);
    drawList->AddText(iconPos, iconColor, icon);
    
    // Draw label
    ImVec2 textPos(pos.x + 25, pos.y + (lineHeight - ImGui::GetTextLineHeight()) * 0.5f);
    drawList->AddText(textPos, IM_COL32(255, 255, 255, 255), item.label.c_str());
    
    // Draw detail (type information) if available
    if (!item.detail.empty()) {
        ImVec2 textSize = ImGui::CalcTextSize(item.label.c_str());
        ImVec2 detailPos(pos.x + 30 + textSize.x + 10, 
                        pos.y + (lineHeight - ImGui::GetTextLineHeight()) * 0.5f);
        drawList->AddText(detailPos, IM_COL32(136, 136, 136, 255), item.detail.c_str());
    }
    
    // Draw documentation tooltip on hover
    if (ImGui::IsItemHovered() && !item.detail.empty()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(item.detail.c_str());
        ImGui::EndTooltip();
    }
    
    ImGui::Dummy(ImVec2(0, lineHeight));
}

// void LSPAutocomplete::renderCompletionListItems()
// {
// 	const int current_item_count = currentCompletionItems.size();
// 	const float max_visible_items = 10.0f;
// 	const float item_height = ImGui::GetTextLineHeightWithSpacing(); // Includes spacing
// 	const ImGuiStyle &style = ImGui::GetStyle();
// 	ImDrawList *draw_list = ImGui::GetWindowDrawList();

// 	const ImU32 selection_color =
// 		ImGui::ColorConvertFloat4ToU32(style.Colors[ImGuiCol_Header]);
// 	const ImU32 text_color = ImGui::ColorConvertFloat4ToU32(style.Colors[ImGuiCol_Text]);

// 	bool use_child_window = current_item_count > max_visible_items;

// 	bool scroll_to_top = use_child_window && showCompletions && !wasShowingLastFrame;

// 	bool selection_changed_by_keyboard =
// 		ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow);

// 	if (use_child_window)
// 	{
// 		float child_height = max_visible_items * item_height;
// 		ImGui::BeginChild("##CompletionScroll", ImVec2(350.0f, child_height), false, 0);
// 		if (scroll_to_top)
// 		{
// 			ImGui::SetScrollY(0.0f);
// 		}
// 	}

// 	for (size_t i = 0; i < currentCompletionItems.size(); ++i)
// 	{
// 		const auto &item = currentCompletionItems[i];
// 		bool is_selected = (selectedCompletionIndex == i);

// 		ImVec2 item_pos = ImGui::GetCursorScreenPos();
// 		float item_width = ImGui::GetContentRegionAvail().x;
// 		float adjusted_item_width =
// 			item_width - (use_child_window ? style.ScrollbarSize : 0.0f);
// 		adjusted_item_width = std::max(1.0f, adjusted_item_width);
// 		ImVec2 item_rect_min = item_pos;
// 		ImVec2 item_rect_max =
// 			ImVec2(item_pos.x + adjusted_item_width, item_pos.y + item_height);

// 		ImGui::Dummy(ImVec2(0.0f, item_height));

// 		if (!ImGui::IsRectVisible(item_pos, item_rect_max))
// 			continue;

// 		if (is_selected)
// 		{
// 			draw_list->AddRectFilled(item_rect_min, item_rect_max, selection_color);
// 		}

// 		ImVec2 text_size = ImGui::CalcTextSize(item.insertText.c_str());
// 		float text_padding_y = (item_height - text_size.y) * 0.5f;
// 		text_padding_y = std::max(0.0f, text_padding_y);
// 		ImVec2 text_pos = ImVec2(item_rect_min.x + style.FramePadding.x,
// 								 item_rect_min.y + text_padding_y);
// 		draw_list->AddText(text_pos, text_color, item.insertText.c_str());

// 		if (is_selected && selection_changed_by_keyboard)
// 		{
// 			ImGui::SetScrollHereY(0.5f);
// 		}
// 	}

// 	if (use_child_window)
// 	{
// 		ImGui::EndChild();
// 	}
// }

void LSPAutocomplete::renderCompletionListItems() {
    const int current_item_count = currentCompletionItems.size();
    const float max_visible_items = 10.0f;
    const float item_height = ImGui::GetTextLineHeightWithSpacing();
    
    bool use_child_window = current_item_count > max_visible_items;
    
    if (use_child_window) {
        float child_height = max_visible_items * item_height;
        ImGui::BeginChild("##CompletionScroll", ImVec2(350.0f, child_height), false, 0);
    }
    
    for (size_t i = 0; i < currentCompletionItems.size(); ++i) {
        const auto &item = currentCompletionItems[i];
        bool is_selected = (selectedCompletionIndex == i);
        
        renderCompletionItem(item, is_selected);
        
        if (is_selected && (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || 
                           ImGui::IsKeyPressed(ImGuiKey_DownArrow))) {
            ImGui::SetScrollHereY(0.5f);
        }
    }
    
    if (use_child_window) {
        ImGui::EndChild();
    }
}

bool LSPAutocomplete::handleClickOutside()
{
	if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
		ImGui::IsMouseClicked(ImGuiMouseButton_Right))
	{
		if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup))
		{
			std::cout << "[renderCompletions] Clicked outside, hiding." << std::endl;
			showCompletions = false;
			editor_state.block_input = false; // Ensure unblocked
			wasShowingLastFrame = false;	  // Reset state
			return true;					  // Indicate closed by click outside
		}
	}
	return false;
}

void LSPAutocomplete::finalizeRenderState()
{
	if (!showCompletions && editor_state.block_input)
	{
		editor_state.block_input = false;
		std::cout << "[renderCompletions] Closed inside Begin/End, ensuring "
					 "input unblocked."
				  << std::endl;
	}
	wasShowingLastFrame = showCompletions;
}

void LSPAutocomplete::renderCompletions()
{
	if (!shouldRender())
	{
		return;
	}

	if (handleInputAndCheckClose())
	{
		return;
	}

	ImVec2 windowSize, safePos;
	calculateWindowGeometry(windowSize, safePos);

	if (showCompletions && !wasShowingLastFrame)
	{
		ImGui::SetNextWindowFocus();
	}

	ImGui::SetNextWindowPos(safePos);
	ImGui::SetNextWindowSize(windowSize);

	applyStyling(); // Now pushes 3 Colors, 3 Vars

	ImGuiWindowFlags windowFlags =
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing |
		ImGuiWindowFlags_NoNav;

	if (ImGui::Begin("##CompletionPopupActual", nullptr, windowFlags))
	{
		renderCompletionListItems(); // Pushes/Pops 1 Color internally
		handleClickOutside();
		ImGui::End();
	} else
	{
		if (editor_state.block_input)
		{
			editor_state.block_input = false;
		}
	}

	ImGui::PopStyleColor(3);
	ImGui::PopStyleVar(3);
	finalizeRenderState();
}

std::string LSPAutocomplete::formCompletionRequest(int requestId,
                                                   const std::string &filePath,
                                                   int line,
                                                   int character)
{
    // Figure out trigger
    char prev_char = (editor_state.cursor_index > 0)
                   ? editor_state.fileContent[editor_state.cursor_index - 1]
                   : '\0';
    int triggerKind = 1; // Invoked
    const char* trig = nullptr;
    if (prev_char == '.' || prev_char == ':' || prev_char == '>') {
        triggerKind = 2;  // TriggerCharacter
        trig = (prev_char == '.') ? "." : (prev_char == ':') ? ":" : ">";
    }

    const std::string uri = pathToFileUri(filePath); // gives file:///D:/... with spaces as %20

    nlohmann::json params = {
        {"textDocument", { {"uri", uri} }},
        {"position",     { {"line", line}, {"character", character} }},
        {"context",      { {"triggerKind", triggerKind} }}
    };
    if (trig) params["context"]["triggerCharacter"] = trig;

    nlohmann::json req = {
        {"jsonrpc","2.0"},
        {"id",     requestId},
        {"method", "textDocument/completion"},
        {"params", params}
    };

    std::string body = req.dump();
    std::cout << "[TX completion] " << body.substr(0, 200) << "...\n";
    return body;
}


bool LSPAutocomplete::processResponse(const std::string &response, int requestId)
{
	try
	{
		json j = json::parse(response);

		// Check if response matches our request ID
		if (!j.contains("id") || !j["id"].is_number_integer() ||
			j["id"].get<int>() != requestId)
		{
			return false;
		}

		std::cout << "\033[32mLSP Autocomplete:\033[0m Received response for ID "
				  << requestId << std::endl;

		// Handle errors
		if (j.contains("error"))
		{
			std::cerr << "\033[31mLSP Autocomplete:\033[0m Error: " << j["error"].dump(2)
					  << std::endl;
			// Clean up failed request
			{
				std::lock_guard<std::mutex> lock(activeRequestsMutex);
				activeRequests.erase(requestId);
			}
			currentCompletionItems.clear();
			showCompletions = false;
			return true;
		}

		// Process result
		if (j.contains("result"))
		{
			// Retrieve original request coordinates
			int requestLine = -1, requestCharacter = -1;
			{
				std::lock_guard<std::mutex> lock(activeRequestsMutex);
				auto it = activeRequests.find(requestId);
				if (it != activeRequests.end())
				{
					requestLine = it->second.line;
					requestCharacter = it->second.character;
					activeRequests.erase(it); // Clean up completed request
				}
			}

			parseCompletionResult(j["result"], requestLine, requestCharacter);
			return true;
		}

		// Handle missing result
		std::cout << "\033[31mLSP Autocomplete:\033[0m Response missing "
					 "'result' field."
				  << std::endl;
		// Clean up failed request
		{
			std::lock_guard<std::mutex> lock(activeRequestsMutex);
			activeRequests.erase(requestId);
		}
		currentCompletionItems.clear();
		showCompletions = false;
		return true;
	} catch (const json::exception &e)
	{
		std::cerr << "\033[31mLSP Autocomplete:\033[0m JSON error: " << e.what()
				  << std::endl;
		// Clean up failed request
		{
			std::lock_guard<std::mutex> lock(activeRequestsMutex);
			activeRequests.erase(requestId);
		}
		currentCompletionItems.clear();
		showCompletions = false;
		return true;
	}
}

bool LSPAutocomplete::shouldIncludeCompletion(const CompletionDisplayItem& item, 
                                             CompletionContext context) {
    // Filter out editor commands and unwanted items
    if (item.label.empty()) return false;
    if (item.label.find("editor.action.") == 0) return false;
    
    // Filter out very generic single-character completions
    if (item.label.length() == 1 && !isalpha(item.label[0])) return false;
    
    // Filter based on context
    switch (context) {
        case CompletionContext::PropertyAccess:
            // In property context, show only methods/properties
            return item.kind == 2 || item.kind == 5; // Functions and Variables
            
        case CompletionContext::FunctionCall:
            // In function call context, prioritize functions
            return item.kind == 2 || item.kind == 3; // Functions, Constructors
            
        case CompletionContext::StringMethod:
            // For string methods, show only string-related methods
            return item.label.find("sub") != std::string::npos ||
                   item.label.find("find") != std::string::npos ||
                   item.label.find("gsub") != std::string::npos ||
                   item.label.find("match") != std::string::npos ||
                   item.label.find("upper") != std::string::npos ||
                   item.label.find("lower") != std::string::npos;
    }
    
    return true;
}

void LSPAutocomplete::prioritizeCompletions(std::vector<CompletionDisplayItem>& items, 
                                           CompletionContext context,
                                           const std::string& currentWord) {
    for (auto& item : items) {
        std::string priorityPrefix = "Z"; // Default low priority
        
        // Context-based prioritization
        switch (context) {
            case CompletionContext::PropertyAccess:
                if (item.kind == 2) priorityPrefix = "A"; // Functions first
                else if (item.kind == 5) priorityPrefix = "B"; // Properties second
                break;
                
            case CompletionContext::FunctionCall:
                if (item.kind == 2) priorityPrefix = "A"; // Functions
                else if (item.kind == 3) priorityPrefix = "B"; // Constructors
                break;
                
            case CompletionContext::Global:
                // Prioritize keywords and common patterns for Roblox development
                if (item.label == "function" || item.label == "local" || 
                    item.label == "game" || item.label == "workspace") {
                    priorityPrefix = "A";
                } else if (item.label.find("local ") == 0) {
                    priorityPrefix = "B";
                } else if (item.label == "print") {
                    priorityPrefix = "C"; // Common but not highest priority
                }
                break;
                
            case CompletionContext::StringMethod:
                // Prioritize common string methods
                if (item.label == "sub" || item.label == "find") {
                    priorityPrefix = "A";
                } else if (item.label == "gsub" || item.label == "match") {
                    priorityPrefix = "B";
                }
                break;
        }
        
        // Exact match boost (highest priority)
        if (!currentWord.empty()) {
            if (item.label.substr(0, currentWord.length()) == currentWord) {
                priorityPrefix = "!" + priorityPrefix; // Exact match
            } else {
                // Case-insensitive prefix match
                std::string labelLower = item.label;
                std::transform(labelLower.begin(), labelLower.end(), labelLower.begin(), ::tolower);
                std::string currentWordLower = currentWord;
                std::transform(currentWordLower.begin(), currentWordLower.end(), currentWordLower.begin(), ::tolower);
                
                if (labelLower.substr(0, currentWordLower.length()) == currentWordLower) {
                    priorityPrefix = "@" + priorityPrefix; // Case-insensitive match
                }
            }
        }
        
        item.sortText = priorityPrefix + item.sortText;
    }
}

//Clean up snippet formatting
std::string LSPAutocomplete::cleanSnippetFormatting(const std::string& text) {
    std::string result = text;

    // --- existing placeholder cleanup (your loop) ---
    size_t pos = 0;
    while ((pos = result.find("$", pos)) != std::string::npos) {
        if (pos + 1 < result.size()) {
            if (result[pos + 1] == '{') {
                size_t end = result.find("}", pos);
                if (end != std::string::npos) {
                    size_t colon = result.find(":", pos);
                    if (colon != std::string::npos && colon < end) {
                        std::string defaultValue = result.substr(colon + 1, end - colon - 1);
                        result.replace(pos, end - pos + 1, defaultValue);
                    } else {
                        result.erase(pos, end - pos + 1);
                    }
                } else {
                    result.erase(pos, 2);
                }
            } else if (isdigit(result[pos + 1])) {
                size_t end = pos + 1;
                while (end < result.size() && isdigit(result[end])) end++;
                result.erase(pos, end - pos);
            } else if (result[pos + 1] == '(') {
                size_t end = result.find(")", pos);
                if (end != std::string::npos) result.erase(pos, end - pos + 1);
                else result.erase(pos, 2);
            } else {
                pos++;
            }
        } else break;
    }

    // --- enforce "open paren only" for function snippets like print() / GetService() ---
    size_t openPos = result.find('(');
    if (openPos != std::string::npos) {
        // keep everything through '(' and drop everything after it
        result = result.substr(0, openPos + 1);
    }

    return result;
}

void LSPAutocomplete::parseCompletionResult(const json &result,
                                            int requestLine,
                                            int requestCharacter) {
    std::vector<json> items_json;
    bool is_incomplete = false;

    if (result.is_array()) {
        items_json = result.get<std::vector<json>>();
    } else if (result.is_object()) {
        if (result.contains("items") && result["items"].is_array()) {
            items_json = result["items"].get<std::vector<json>>();
        }
        is_incomplete = result.value("isIncomplete", false);
    } else if (result.is_null()) {
        std::cout << "\033[33mLSP Autocomplete:\033[0m No completions found "
                     "(result is null)."
                  << std::endl;
        currentCompletionItems.clear();
        showCompletions = false;
        return;
    } else {
        std::cout << "\033[31mLSP Autocomplete:\033[0m Unexpected result format: "
                  << result.type_name() << std::endl;
        currentCompletionItems.clear();
        showCompletions = false;
        return;
    }

    std::cout << "\033[32mFound " << items_json.size() << " completions"
              << (is_incomplete ? " (incomplete list)" : "") << ":\033[0m" << std::endl;

    // Use the original request coordinates that were sent to server
    int currentLine = requestLine;
    int currentChar = requestCharacter;

    // Calculate the original cursor position when the request was made
    int request_cursor_pos = -1;
    if (currentLine >= 0 && currentLine < editor_state.editor_content_lines.size()) {
        int line_start = editor_state.editor_content_lines[currentLine];
        request_cursor_pos = line_start + currentChar;

        // Bounds check to ensure we don't exceed file content
        if (request_cursor_pos < 0 || request_cursor_pos > editor_state.fileContent.size()) {
            request_cursor_pos = editor_state.cursor_index;
            auto [line, character] = getLineAndCharFromIndex(request_cursor_pos);
            currentLine = line;
            currentChar = character;
        }
    } else {
        // Fallback to current cursor if coordinates are invalid
        request_cursor_pos = editor_state.cursor_index;
        auto [line, character] = getLineAndCharFromIndex(request_cursor_pos);
        currentLine = line;
        currentChar = character;
    }

    // Find the start of the current word for filtering purposes
    std::string currentWord;
    int word_start = request_cursor_pos;

    // Smart word boundary detection for property access
    bool isPropertyAccess = false;
    int lastAccessorPos = -1;

    // Look backwards to find the last '.' or ':'
    for (int i = request_cursor_pos - 1; i >= 0; i--) {
        char c = editor_state.fileContent[i];
        if (c == '.' || c == ':') {
            lastAccessorPos = i;
            isPropertyAccess = true;
            break;
        }
        if (!isalnum(c) && c != '_') break; // stop on non-identifier
    }

    if (isPropertyAccess && lastAccessorPos != -1) {
        // word starts after the last accessor
        word_start = lastAccessorPos + 1;
    } else {
        const std::string additionalWordChars = ":$#@";
        while (word_start > 0) {
            char c = editor_state.fileContent[word_start - 1];
            if (!(isalnum(c) || c == '_' || additionalWordChars.find(c) != std::string::npos))
                break;
            word_start--;
        }
    }

    if (word_start < request_cursor_pos && word_start >= 0 &&
        request_cursor_pos <= editor_state.fileContent.size()) {
        currentWord =
            editor_state.fileContent.substr(word_start, request_cursor_pos - word_start);
    }

    // Detect completion context
    CompletionContext context = detectCompletionContext(request_cursor_pos);
    
    // Debug output for context detection
    const char* contextNames[] = {"Global", "PropertyAccess", "FunctionCall", 
                                 "TableAccess", "StringMethod", "Unknown"};
    std::cout << "Completion context: " << contextNames[static_cast<int>(context)] 
              << ", current word: '" << currentWord << "'" << std::endl;

    // Use a map to deduplicate completions while preserving the best one
    std::unordered_map<std::string, CompletionDisplayItem> uniqueItems;

    for (const auto &item_json : items_json) {
        CompletionDisplayItem newItem;
        newItem.label = item_json.value("label", "[No Label]");
        newItem.detail = item_json.value("detail", "");
        newItem.kind = item_json.value("kind", 0);
        newItem.startLine = -1;
        newItem.startChar = -1;
        newItem.endLine = -1;
        newItem.endChar = -1;

        // Use server's sortText if available
        newItem.sortText = item_json.value("sortText", newItem.label);

        // Create a unique key that preserves important distinctions
        std::string uniqueKey = newItem.label + "|" + std::to_string(newItem.kind);

        // Only replace if we have a better sortText
        auto it = uniqueItems.find(uniqueKey);
        if (it == uniqueItems.end() || newItem.sortText < it->second.sortText) {
            bool hasTextEdit = false;

            // First try to get textEdit data
            if (item_json.contains("textEdit") && item_json["textEdit"].is_object()) {
                const auto &textEdit = item_json["textEdit"];
                if (textEdit.contains("newText") && textEdit["newText"].is_string()) {
                    newItem.insertText = textEdit["newText"].get<std::string>();
					newItem.insertText = cleanSnippetFormatting(newItem.insertText);

                    if (textEdit.contains("range") && textEdit["range"].is_object()) {
                        const auto &range = textEdit["range"];
                        if (range.contains("start") && range["start"].is_object() &&
                            range.contains("end") && range["end"].is_object()) {
                            const auto &start = range["start"];
                            const auto &end = range["end"];

                            newItem.startLine = start.value("line", -1);
                            newItem.startChar = start.value("character", -1);
                            newItem.endLine = end.value("line", -1);
                            newItem.endChar = end.value("character", -1);
                        }
                    }
                    hasTextEdit = true;
                }
            }


            // If no textEdit OR textEdit has invalid coordinates, use server coordinates
            if (!hasTextEdit || newItem.startLine == -1 || newItem.startChar == -1 ||
                newItem.endLine == -1 || newItem.endChar == -1) {
                // According to LSP spec, when no textEdit is provided, the
                // completion replaces from the start of the current word to the
                // cursor position
                auto [wordStartLine, wordStartChar] = getLineAndCharFromIndex(word_start);

                newItem.startLine = wordStartLine;
                newItem.startChar = wordStartChar;
                newItem.endLine = currentLine;
                newItem.endChar = currentChar;

                // Get insert text from either insertText or label
                if (item_json.contains("insertText") &&
                    item_json["insertText"].is_string()) {
                    newItem.insertText = item_json["insertText"].get<std::string>();
					newItem.insertText = cleanSnippetFormatting(newItem.insertText);
                } else {
					newItem.insertText = cleanSnippetFormatting(newItem.insertText);
                    newItem.insertText = newItem.label;
                }
            }

            // Apply context-aware filtering
            if (shouldIncludeCompletion(newItem, context)) {
                // Apply prioritization scoring
                std::string priorityPrefix = "Z"; // Default low priority
                
                // Context-based prioritization
                switch (context) {
                    case CompletionContext::PropertyAccess:
                        if (newItem.kind == 2) priorityPrefix = "A"; // Functions first
                        else if (newItem.kind == 5) priorityPrefix = "B"; // Properties second
                        break;
                        
                    case CompletionContext::FunctionCall:
                        if (newItem.kind == 2) priorityPrefix = "A"; // Functions
                        else if (newItem.kind == 3) priorityPrefix = "B"; // Constructors
                        break;
                        
                    case CompletionContext::Global:
                        // Prioritize keywords and common patterns for Roblox
                        if (newItem.label == "function" || newItem.label == "local" || 
                            newItem.label == "game" || newItem.label == "workspace") {
                            priorityPrefix = "A";
                        } else if (newItem.label.find("local ") == 0) {
                            priorityPrefix = "B";
                        } else if (newItem.label == "print") {
                            priorityPrefix = "C";
                        }
                        break;
                        
                    case CompletionContext::StringMethod:
                        // Prioritize common string methods
                        if (newItem.label == "sub" || newItem.label == "find") {
                            priorityPrefix = "A";
                        } else if (newItem.label == "gsub" || newItem.label == "match") {
                            priorityPrefix = "B";
                        }
                        break;
                        
                    default:
                        break;
                }
                
                // Exact match boost (highest priority)
                if (!currentWord.empty()) {
                    if (newItem.label.substr(0, currentWord.length()) == currentWord) {
                        priorityPrefix = "!" + priorityPrefix; // Exact match
                    } else {
                        // Case-insensitive prefix match
                        std::string labelLower = newItem.label;
                        std::transform(labelLower.begin(), labelLower.end(), labelLower.begin(), ::tolower);
                        std::string currentWordLower = currentWord;
                        std::transform(currentWordLower.begin(), currentWordLower.end(), currentWordLower.begin(), ::tolower);
                        
                        if (labelLower.substr(0, currentWordLower.length()) == currentWordLower) {
                            priorityPrefix = "@" + priorityPrefix; // Case-insensitive match
                        } else if (labelLower.find(currentWordLower) != std::string::npos) {
                            priorityPrefix = "#" + priorityPrefix; // Contains match
                        }
                    }
                }
                
                newItem.sortText = priorityPrefix + newItem.sortText;
                uniqueItems[uniqueKey] = newItem;
            }
        }
    }

    // Convert map to vector
    currentCompletionItems.clear();
    currentCompletionItems.reserve(uniqueItems.size());
    for (const auto &pair : uniqueItems) {
        currentCompletionItems.push_back(pair.second);
    }

    // Sort completions using our enhanced sortText
    std::sort(currentCompletionItems.begin(),
              currentCompletionItems.end(),
              [](const CompletionDisplayItem &a, const CompletionDisplayItem &b) {
                  return a.sortText < b.sortText;
              });

    // Limit to top items for performance and usability
    const size_t MAX_COMPLETIONS = 25;
    if (currentCompletionItems.size() > MAX_COMPLETIONS) {
        currentCompletionItems.resize(MAX_COMPLETIONS);
    }

    // Debug output for filtered results
    std::cout << "Filtered to " << currentCompletionItems.size() << " relevant completions:" << std::endl;
    for (size_t i = 0; i < std::min(currentCompletionItems.size(), size_t(5)); ++i) {
        const auto& item = currentCompletionItems[i];
        std::cout << "  " << item.sortText << " - " << item.label 
                  << " (kind: " << item.kind << ")" << std::endl;
    }
    if (currentCompletionItems.size() > 5) {
        std::cout << "  ... and " << (currentCompletionItems.size() - 5) << " more" << std::endl;
    }

    // Update UI state
    if (!currentCompletionItems.empty()) {
        updatePopupPosition();
        showCompletions = true;
        selectedCompletionIndex = 0;
        
        // Auto-select the best match if we have an exact prefix match
        if (!currentWord.empty()) {
            for (size_t i = 0; i < currentCompletionItems.size(); ++i) {
                const auto& item = currentCompletionItems[i];
                if (item.label.substr(0, currentWord.length()) == currentWord) {
                    selectedCompletionIndex = i;
                    break;
                }
            }
        }
    } else {
        showCompletions = false;
    }
}

void LSPAutocomplete::resetPopupPosition()
{
	lastPositionUpdate = std::chrono::steady_clock::time_point::min();
}

void LSPAutocomplete::updatePopupPosition()
{
	try
	{
		auto now = std::chrono::steady_clock::now();
		auto timeSinceLastUpdate = std::chrono::duration_cast<std::chrono::milliseconds>(
									   now - lastPositionUpdate)
									   .count();

		// Only update position if it's been more than 2 seconds or we don't
		// have a cached position
		if (timeSinceLastUpdate > POSITION_CACHE_DURATION_MS ||
			lastPositionUpdate == std::chrono::steady_clock::time_point::min())
		{

			int cursor_line = gEditor.getLineFromPos(editor_state.cursor_index);
			float cursor_x = gEditorCursor.getCursorXPosition(editor_state.text_pos,
															  editor_state.fileContent,
															  editor_state.cursor_index);
			completionPopupPos = editor_state.text_pos;
			completionPopupPos.x = cursor_x;
			completionPopupPos.y += cursor_line * editor_state.line_height;

			// Cache the new position and timestamp
			lastPopupPos = completionPopupPos;
			lastPositionUpdate = now;

			std::cout << ">>> [requestCompletion] Updated Anchor Pos: ("
					  << completionPopupPos.x << ", " << completionPopupPos.y << ")"
					  << std::endl;
		} else
		{
			// Reuse the cached position
			completionPopupPos = lastPopupPos;
		}
	} catch (const std::exception &e)
	{
		std::cerr << "!!! ERROR calculating cursor pos: " << e.what() << std::endl;
		completionPopupPos = ImVec2(0, 0);
		lastPopupPos = completionPopupPos;
		lastPositionUpdate = std::chrono::steady_clock::now();
	}
}