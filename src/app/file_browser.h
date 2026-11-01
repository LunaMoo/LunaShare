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

private:
    void Refresh();
    void Navigate(const std::string& dir);
    bool open_ = false, requestOpen_ = false;
    Mode mode_ = Mode::OpenMulti;
    std::string title_, dir_, filter_, error_;
    std::string pathBuf_, nameBuf_, newFolderBuf_;
    std::vector<ls::DirEntry> entries_;
    std::set<std::string> selected_;
    int lastClicked_ = -1;
    bool showHidden_ = false;
    std::vector<std::pair<std::string, std::string>> places_;
};
