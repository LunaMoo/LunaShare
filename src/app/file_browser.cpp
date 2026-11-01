#include "file_browser.h"

#include <algorithm>

#include "imgui.h"
#include "imgui_stdlib.h"
#include "ui_widgets.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace ls;

static std::vector<std::pair<std::string, std::string>> BuildPlaces() {
    std::vector<std::pair<std::string, std::string>> p;
    std::string home = HomeDir();
#if defined(__ANDROID__)
    p.push_back({"Phone storage", "/storage/emulated/0"});
    p.push_back({"Download", "/storage/emulated/0/Download"});
    p.push_back({"Camera", "/storage/emulated/0/DCIM"});
    p.push_back({"Pictures", "/storage/emulated/0/Pictures"});
    p.push_back({"Documents", "/storage/emulated/0/Documents"});
    for (auto& e : ListDir("/storage"))
        if (e.isDir && e.name != "emulated" && e.name != "self") p.push_back({"SD card " + e.name, "/storage/" + e.name});
#else
    p.push_back({"Home", home});
    for (const char* sub : {"Desktop", "Documents", "Downloads", "Pictures", "Videos", "Music"}) {
        std::string d = JoinPath(home, sub);
        if (IsDirectory(d)) p.push_back({sub, d});
    }
#ifdef _WIN32
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++)
        if (mask & (1u << i)) {
            std::string d = std::string(1, (char)('A' + i)) + ":\\";
            p.push_back({d, d});
        }
#else
    p.push_back({"/", "/"});
#if defined(__APPLE__)
    p.push_back({"Volumes", "/Volumes"});
#else
    if (IsDirectory("/media")) p.push_back({"Media", "/media"});
    if (IsDirectory("/mnt")) p.push_back({"mnt", "/mnt"});
#endif
#endif
#endif
    return p;
}

void FileBrowser::Open(Mode mode, const std::string& title, const std::string& startDir, const std::string& defaultName,
                       const std::string& extFilter) {
    mode_ = mode;
    title_ = title;
    filter_ = extFilter;
    nameBuf_ = defaultName;
    selected_.clear();
    result.clear();
    error_.clear();
    places_ = BuildPlaces();
    std::string d = startDir;
    if (d.empty() || !IsDirectory(d)) d = !lastDir.empty() && IsDirectory(lastDir) ? lastDir : HomeDir();
    Navigate(d);
    requestOpen_ = true;
    open_ = true;
}

void FileBrowser::Navigate(const std::string& dir) {
    if (!IsDirectory(dir)) {
        error_ = "Cannot open " + dir;
        return;
    }
    dir_ = dir;
    pathBuf_ = dir;
    lastDir = dir;
    selected_.clear();
    lastClicked_ = -1;
    error_.clear();
    Refresh();
}

void FileBrowser::Refresh() {
    entries_.clear();
    for (auto& e : ListDir(dir_)) {
        if (!showHidden_ && !e.name.empty() && e.name[0] == '.') continue;
        if (!e.isDir && !filter_.empty()) {
            std::string n = e.name;
            std::transform(n.begin(), n.end(), n.begin(), ::tolower);
            if (n.size() < filter_.size() || n.compare(n.size() - filter_.size(), filter_.size(), filter_) != 0) continue;
        }
        if (mode_ == Mode::SelectDir && !e.isDir) continue;
        entries_.push_back(e);
    }
}

bool FileBrowser::Draw() {
    if (!open_) return false;
    bool mobile = ui::M().mobile;
    const char* popupId = "##filebrowser";
    if (requestOpen_) {
        ImGui::OpenPopup(popupId);
        requestOpen_ = false;
    }
    ImGuiViewport* vp = ImGui::GetMainViewport();
    if (mobile) {
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
    } else {
        ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.7f, vp->WorkSize.y * 0.75f), ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    }
    bool confirmed = false;
    bool keepOpen = true;
    ImGuiWindowFlags wf = mobile ? (ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar) : 0;
    if (!ImGui::BeginPopupModal(popupId, mobile ? nullptr : &keepOpen, wf)) {
        open_ = false;
        return false;
    }
    if (mobile) {
        ImGui::TextUnformatted(title_.c_str());
        ImGui::Separator();
    } else {
        ImGui::TextUnformatted(title_.c_str());
    }

    // ---- toolbar
    if (ui::IconButton("up", ui::Icon::Up, "Parent folder")) {
        std::string parent = DirName(dir_);
        if (!parent.empty() && parent != dir_) Navigate(parent);
    }
    ImGui::SameLine();
    if (ui::IconButton("refresh", ui::Icon::Retry, "Refresh")) Refresh();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##path", &pathBuf_, ImGuiInputTextFlags_EnterReturnsTrue)) Navigate(pathBuf_);

    float footerH = (mobile ? ui::M().rowH : ImGui::GetFrameHeight()) * (mode_ == Mode::SaveFile ? 2.4f : 1.3f) + ImGui::GetStyle().ItemSpacing.y * 3;
    float placesW = mobile ? 0 : ImGui::GetFontSize() * 11;

    if (mobile) {
        // places as a horizontal combo to save space
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##places", "Go to...")) {
            for (auto& [label, path] : places_)
                if (ImGui::Selectable(label.c_str(), false, 0, ImVec2(0, ui::M().rowH * 0.8f))) Navigate(path);
            ImGui::EndCombo();
        }
    } else {
        ImGui::BeginChild("places", ImVec2(placesW, -footerH), ImGuiChildFlags_Borders);
        for (auto& [label, path] : places_)
            if (ImGui::Selectable(label.c_str(), dir_ == path)) Navigate(path);
        ImGui::EndChild();
        ImGui::SameLine();
    }

    ImGui::BeginChild("entries", ImVec2(0, -footerH), ImGuiChildFlags_Borders);
    if (mobile) ui::TouchScroll();
    if (!error_.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", error_.c_str());
    ImGuiIO& io = ImGui::GetIO();
    std::string navigateTo;
    float rowH = mobile ? ui::M().rowH * 0.9f : 0;
    if (ImGui::BeginTable("files", mobile ? 2 : 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 4.0f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        if (!mobile) ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableHeadersRow();
        if (mobile) ui::TouchScroll();
        for (int i = 0; i < (int)entries_.size(); i++) {
            const DirEntry& e = entries_[(size_t)i];
            std::string full = JoinPath(dir_, e.name);
            bool sel = selected_.count(full) > 0;
            ImGui::TableNextRow(0, rowH);
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            float ih = ImGui::GetTextLineHeight();
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(ih, ih));
            ui::DrawIcon(ImGui::GetWindowDrawList(), e.isDir ? ui::Icon::Folder : ui::Icon::File, ImVec2(p.x + ih * 0.5f, p.y + ih * 0.5f), ih * 0.8f,
                         e.isDir ? ui::ColWarn() : ImGui::GetColorU32(ImGuiCol_Text));
            ImGui::SameLine();
            if (ImGui::Selectable(e.name.c_str(), sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, rowH))) {
                bool multi = mode_ == Mode::OpenMulti;
                if (e.isDir && (mobile || ImGui::IsMouseDoubleClicked(0)) && !(multi && (io.KeyCtrl || io.KeyShift))) {
                    navigateTo = full;
                } else if (multi && io.KeyShift && lastClicked_ >= 0) {
                    int a = std::min(lastClicked_, i), b = std::max(lastClicked_, i);
                    for (int k = a; k <= b; k++) selected_.insert(JoinPath(dir_, entries_[(size_t)k].name));
                } else if (multi && (io.KeyCtrl || mobile)) {
                    if (sel) selected_.erase(full); else selected_.insert(full);
                    lastClicked_ = i;
                } else {
                    selected_.clear();
                    selected_.insert(full);
                    lastClicked_ = i;
                    if (!e.isDir) nameBuf_ = e.name;
                    if (!e.isDir && ImGui::IsMouseDoubleClicked(0) && mode_ != Mode::OpenMulti) confirmed = true;
                    if (!e.isDir && ImGui::IsMouseDoubleClicked(0) && mode_ == Mode::OpenMulti) confirmed = true;
                }
            }
            if (mobile && e.isDir && mode_ == Mode::OpenMulti) {
                // on phones a tap opens the folder; long press selects the whole folder
                if (ImGui::IsItemHovered() && ImGui::IsMouseDown(0) && io.MouseDownDuration[0] > 0.6f && ImGui::GetMouseDragDelta(0).y == 0) {
                    navigateTo.clear();
                    selected_.insert(full);
                }
            }
            ImGui::PopID();
            ImGui::TableNextColumn();
            if (!e.isDir) ImGui::TextDisabled("%s", FormatBytes((double)e.size).c_str());
            if (!mobile) {
                ImGui::TableNextColumn();
                if (e.isDir) ImGui::TextDisabled("folder");
                else {
                    size_t dot = e.name.find_last_of('.');
                    ImGui::TextDisabled("%s", dot == std::string::npos ? "" : e.name.substr(dot + 1).c_str());
                }
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    if (!navigateTo.empty()) Navigate(navigateTo);

    // ---- footer
    if (mode_ == Mode::SaveFile) {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##name", "File name", &nameBuf_);
    }
    if (mode_ == Mode::SelectDir) {
        ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetContentRegionAvail().x * 0.5f);
        ImGui::InputTextWithHint("##newfolder", "New folder name", &newFolderBuf_);
        if (!mobile) ImGui::SameLine();
        if (ImGui::Button("Create folder") && !newFolderBuf_.empty()) {
            std::string nd = JoinPath(dir_, SanitizeRelativePath(newFolderBuf_));
            if (MakeDirs(nd)) { newFolderBuf_.clear(); Navigate(nd); }
        }
    }
    const char* okLabel = mode_ == Mode::SaveFile ? "Save" : mode_ == Mode::SelectDir ? "Use this folder" : "Open";
    std::string okText = okLabel;
    if (mode_ == Mode::OpenMulti && selected_.size() > 1) okText = "Send " + std::to_string(selected_.size()) + " items";
    else if (mode_ == Mode::OpenMulti) okText = "Send";
    float bw = mobile ? (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f : ImGui::GetFontSize() * 9;
    float bh = mobile ? ui::M().rowH : 0;
    bool canOk = mode_ == Mode::SelectDir || (mode_ == Mode::SaveFile && !nameBuf_.empty()) || !selected_.empty();
    if (!mobile) ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - bw * 2 - ImGui::GetStyle().ItemSpacing.x);
    ImGui::BeginDisabled(!canOk);
    if (ImGui::Button(okText.c_str(), ImVec2(bw, bh))) confirmed = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(bw, bh)) || (!mobile && ImGui::IsKeyPressed(ImGuiKey_Escape))) keepOpen = false;
    if (ImGui::IsKeyPressed(ImGuiKey_AppBack)) keepOpen = false;

    if (confirmed) {
        result.clear();
        if (mode_ == Mode::SelectDir) {
            result.push_back(selected_.size() == 1 && IsDirectory(*selected_.begin()) ? *selected_.begin() : dir_);
        } else if (mode_ == Mode::SaveFile) {
            std::string name = nameBuf_;
            if (!filter_.empty() && (name.size() < filter_.size() || name.substr(name.size() - filter_.size()) != filter_)) name += filter_;
            result.push_back(JoinPath(dir_, name));
        } else {
            for (auto& s : selected_)
                if (mode_ == Mode::OpenMulti || !IsDirectory(s)) result.push_back(s);
        }
        if (result.empty()) confirmed = false;
        else keepOpen = false;
    }
    if (!keepOpen) {
        ImGui::CloseCurrentPopup();
        open_ = false;
    }
    ImGui::EndPopup();
    return confirmed;
}
