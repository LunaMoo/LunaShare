// Cross-platform Dear ImGui file browser (works the same on desktop and phones).
#pragma once
#include <set>
#include <string>
#include <vector>

#include "platform.h"

class FileBrowser {
public:
    enum class Mode { OpenMulti, OpenFile, SelectDir, SaveFile };

    void Open(Mode mode, const std::string& title, const std::string& startDir, const std::string& defaultName = "",
              const std::string& extFilter = "");
    bool IsOpen() const { return open_; }
    // Draws the browser (if open). Returns true once when the user confirmed; read `result`.
    bool Draw();
    std::vector<std::string> result;
    std::string lastDir;

    // View preferences (the app persists them in Settings).
    struct Prefs {
        bool grid = false;
        bool picturesOnly = false;
        int sort = 0;    // 0 name, 1 newest first, 2 largest first
        int tile = 130;  // unscaled tile size
        bool operator!=(const Prefs& o) const { return grid != o.grid || picturesOnly != o.picturesOnly || sort != o.sort || tile != o.tile; }
    } prefs;
    bool prefsChanged = false;
    void SelectFirstFiles(int n);  // used by automated screenshots

private:
    void Refresh();
    void Navigate(const std::string& dir);
    void DrawList(std::string& navigateTo, bool& confirmed);
    void DrawGrid(std::string& navigateTo, bool& confirmed);
    void Click(int i, bool& confirmed, std::string& navigateTo, bool doubleClick);
    int64_t SelectedBytes() const;
    bool open_ = false, requestOpen_ = false;
    Mode mode_ = Mode::OpenMulti;
    std::string title_, dir_, filter_, error_;
    std::string pathBuf_, nameBuf_, newFolderBuf_;
    std::vector<ls::DirEntry> entries_;
    std::set<std::string> selected_;
    int lastClicked_ = -1;
    bool longPressHandled_ = false;
    bool resetScroll_ = false;
    bool openPreview_ = false;
    std::string previewPath_;
    bool showHidden_ = false;
    std::vector<std::pair<std::string, std::string>> places_;
};
