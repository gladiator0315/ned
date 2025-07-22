// keybinds.h
#pragma once
#include "../lib/json.hpp"
#include "imgui.h" // <<< ADD THIS for ImGuiKey
#include "settings_file_manager.h"
#include <filesystem>
#include <map> // <<< ADD THIS for std::map
#include <string>

namespace fs = std::filesystem;
using json = nlohmann::json;

class KeybindsManager
{
  public:
	KeybindsManager();

	bool loadKeybinds();
	void printKeybinds() const;
	void checkKeybindsFile();

	// Getter for the raw JSON keybinds (can still be useful for inspection)
	const json &getRawKeybinds() const { return keybinds_; }

	// Getter for the processed ImGuiKey map
	// Returns ImGuiKey_None if the actionName is not found or key is unmapped
	ImGuiKey getActionKey(const std::string &actionName) const;

  private:
	std::string getUserKeybindsFilePath();
	void ensureUserKeybindsFileExists();
	bool loadKeybindsFromFile(const std::string &filePath);
	void updateLastModificationTime();

	// Helper to convert string to ImGuiKey
	static ImGuiKey stringToImGuiKey(const std::string &keyString); // <<< ADD THIS

	// Helper to process raw JSON keybinds into the ImGuiKey map
	void processKeybinds(); // <<< ADD THIS

	json keybinds_; // Raw JSON data from the file
	std::map<std::string, ImGuiKey>
		processedKeybinds_; // <<< ADD THIS (Action name -> ImGuiKey)

	std::string keybindsFilePath_;
	SettingsFileManager settingsFileManager_;
	fs::file_time_type lastKeybindsModificationTime_;

	int checkFrameCounter_ = 0;
	static const int CHECK_INTERVAL_FRAMES = 60;

	const std::string KEYBINDS_FILENAME_ = "keybinds.json";
};

extern KeybindsManager gKeybinds;
