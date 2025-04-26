// file_finder.h

#pragma once
#include "files.h"
#include "imgui.h"
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

struct FileEntry
{
	std::string fullPath;
	std::string relativePath;
	std::string fullPathLower; // Lower-case full path for searching
	std::string filenameLower; // Lower-case file name (from relativePath.filename())
};

class FileFinder
{
  private:
	char searchBuffer[256] = ""; // Buffer for the search input
	bool wasKeyboardFocusSet = false;

	std::string previousSearch;
	std::string originalFile;
	std::string currentlyLoadedFile;

	std::vector<FileEntry> fileList;
	std::vector<FileEntry> filteredList;
	bool isInitialSelection = true; // Track if this is the first selection after opening

	int selectedIndex = 0;
	void updateFilteredList();
	std::thread workerThread;
	std::mutex fileListMutex;
	std::atomic<bool> stopThread{false};
	std::string currentProjectDir;

	void backgroundRefresh();
	void refreshFileListBackground(const std::string &projectDir);
	// Helper functions to break up the renderWindow() logic:
	void renderHeader();
	bool renderSearchInput();
	void renderFileList();

	void handleSelectionChange();
	int orginal_cursor_index;

	std::chrono::steady_clock::time_point lastSelectionTime;
	std::string pendingFile;
	bool hasPendingSelection = false;

	void checkPendingSelection(); // Add this declaration

  public:
	bool showFFWindow = false;
	void toggleWindow();
	bool isWindowOpen() const;
	void renderWindow();
	FileFinder();
	~FileFinder();
};

extern FileFinder gFileFinder;
