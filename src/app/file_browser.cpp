#include "file_browser.h"
#include "i18n.h"

#include <algorithm>
#include <cmath>

#include "image_cache.h"
#include "imgui.h"
#include "imgui_internal.h"
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
    p.push_back({T("Phone storage"), "/storage/emulated/0"});
    p.push_back({T("Camera"), "/storage/emulated/0/DCIM/Camera"});
    p.push_back({T("Pictures"), "/storage/emulated/0/Pictures"});
    p.push_back({T("Screenshots"), "/storage/emulated/0/Pictures/Screenshots"});
    p.push_back({T("Download"), "/storage/emulated/0/Download"});
    p.push_back({T("Documents"), "/storage/emulated/0/Documents"});
    for (auto& e : ListDir("/storage"))
        if (e.isDir && e.name != "emulated" && e.name != "self") p.push_back({TF("SD card %s", e.name.c_str()), "/storage/" + e.name});
    p.erase(std::remove_if(p.begin(), p.end(), [](auto& x) { return !IsDirectory(x.second); }), p.end());
#else
    p.push_back({T("Home"), home});
    for (const char* sub : {"Desktop", "Documents", "Downloads", "Pictures", "Videos", "Music"}) {
        std::string d = JoinPath(home, sub);
        if (IsDirectory(d)) p.push_back({T(sub), d});
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
    p.push_back({T("Volumes"), "/Volumes"});
#else
    if (IsDirectory("/media")) p.push_back({T("Media"), "/media"});
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
        error_ = TF("Cannot open %s", dir.c_str());
        return;
    }
    dir_ = dir;
    pathBuf_ = dir;
    lastDir = dir;
    selected_.clear();
    lastClicked_ = -1;
    error_.clear();
    Refresh();
    resetScroll_ = true;
}

void FileBrowser::Refresh() {
    entries_.clear();
    bool galleryFilter = prefs.picturesOnly && mode_ == Mode::OpenMulti;
    for (auto& e : ListDir(dir_)) {
        if (!showHidden_ && !e.name.empty() && e.name[0] == '.') continue;
        if (!e.isDir && !filter_.empty()) {
            std::string n = e.name;
            std::transform(n.begin(), n.end(), n.begin(), ::tolower);
            if (n.size() < filter_.size() || n.compare(n.size() - filter_.size(), filter_.size(), filter_) != 0) continue;
        }
        if (mode_ == Mode::SelectDir && !e.isDir) continue;
        if (galleryFilter && !e.isDir && !ImageCache::IsImagePath(e.name)) continue;
        entries_.push_back(e);
    }
    int sort = prefs.sort;
    std::stable_sort(entries_.begin(), entries_.end(), [sort](const DirEntry& a, const DirEntry& b) {
        if (a.isDir != b.isDir) return a.isDir;
        if (sort == 1 && a.mtime != b.mtime) return a.mtime > b.mtime;
        if (sort == 2 && !a.isDir && a.size != b.size) return a.size > b.size;
        return false;  // ListDir already sorted by name
    });
}

void FileBrowser::SelectFirstFiles(int n) {
    for (auto& e : entries_)
        if (!e.isDir && n-- > 0) selected_.insert(JoinPath(dir_, e.name));
}

int64_t FileBrowser::SelectedBytes() const {
    int64_t total = 0;
    for (auto& e : entries_)
        if (!e.isDir && selected_.count(JoinPath(dir_, e.name))) total += e.size;
    return total;
}

void FileBrowser::Click(int i, bool& confirmed, std::string& navigateTo, bool doubleClick) {
    ImGuiIO& io = ImGui::GetIO();
    bool mobile = ui::M().mobile;
    const DirEntry& e = entries_[(size_t)i];
    std::string full = JoinPath(dir_, e.name);
    bool sel = selected_.count(full) > 0;
    bool multi = mode_ == Mode::OpenMulti;
    if (e.isDir && (mobile || doubleClick) && !(multi && (io.KeyCtrl || io.KeyShift))) {
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
        if (!e.isDir && doubleClick) confirmed = true;
    }
}

// ---------------------------------------------------------------- list view
void FileBrowser::DrawList(std::string& navigateTo, bool& confirmed) {
    bool mobile = ui::M().mobile;
    ImGuiIO& io = ImGui::GetIO();
    float rowH = mobile ? ui::M().rowH * 0.9f : 0;
    if (!ImGui::BeginTable("files", mobile ? 2 : 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(T("Name"), ImGuiTableColumnFlags_WidthStretch, 4.0f);
    ImGui::TableSetupColumn(T("Size"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
    if (!mobile) {
        ImGui::TableSetupColumn(T("Modified"), ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn(T("Type"), ImGuiTableColumnFlags_WidthStretch, 0.7f);
    }
    ImGui::TableHeadersRow();
    if (mobile) ui::TouchScroll();
    ImGuiListClipper clip;
    clip.Begin((int)entries_.size());
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            const DirEntry& e = entries_[(size_t)i];
            std::string full = JoinPath(dir_, e.name);
            bool sel = selected_.count(full) > 0;
            ImGui::TableNextRow(0, rowH);
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            float ih = ImGui::GetTextLineHeight();
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(ih, ih));
            bool isImg = !e.isDir && ImageCache::IsImagePath(e.name);
            ui::DrawIcon(ImGui::GetWindowDrawList(), e.isDir ? ui::Icon::Folder : isImg ? ui::Icon::Image : ui::Icon::File,
                         ImVec2(p.x + ih * 0.5f, p.y + ih * 0.5f), ih * 0.8f, e.isDir ? ui::ColWarn() : ImGui::GetColorU32(ImGuiCol_Text));
            ImGui::SameLine();
            if (ImGui::Selectable(e.name.c_str(), sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, rowH))) {
                if (!longPressHandled_) Click(i, confirmed, navigateTo, ImGui::IsMouseDoubleClicked(0));
                longPressHandled_ = false;
            }
            if (isImg && !mobile && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                const ImageTex& t = Images().Get(full, ImageCache::Size::Tile);
                if (t.state == ImageTex::Ready && ImGui::BeginTooltip()) {
                    float s = ImGui::GetFontSize() * 10 / (float)std::max(t.w, t.h);
                    ImGui::Image(ImTextureRef(t.Id()), ImVec2((float)t.w * s, (float)t.h * s));
                    ImGui::EndTooltip();
                }
            }
            if (mobile && e.isDir && mode_ == Mode::OpenMulti && ImGui::IsItemActive() && io.MouseDownDuration[0] > 0.6f &&
                ImGui::GetMouseDragDelta(0).y == 0 && !longPressHandled_) {
                // long press selects the whole folder instead of opening it
                if (sel) selected_.erase(full); else selected_.insert(full);
                longPressHandled_ = true;
            }
            ImGui::PopID();
            ImGui::TableNextColumn();
            if (!e.isDir) ImGui::TextDisabled("%s", FormatBytes((double)e.size).c_str());
            if (!mobile) {
                ImGui::TableNextColumn();
                if (e.mtime) ImGui::TextDisabled("%s", FormatTime(e.mtime * 1000).c_str());
                ImGui::TableNextColumn();
                if (e.isDir) ImGui::TextDisabled("folder");
                else {
                    size_t dot = e.name.find_last_of('.');
                    ImGui::TextDisabled("%s", dot == std::string::npos ? "" : e.name.substr(dot + 1).c_str());
                }
            }
        }
    }
    ImGui::EndTable();
}

// "IMG_20261001_120000.jpg" -> "IMG_20...20000.jpg": camera names differ at the end, so keep both ends.
static std::string MiddleEllipsis(const std::string& s, float maxW) {
    if (ImGui::CalcTextSize(s.c_str()).x <= maxW) return s;
    std::vector<size_t> cps;  // code point starts
    for (size_t i = 0; i < s.size(); i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80) cps.push_back(i);
    cps.push_back(s.size());
    size_t n = cps.size() - 1;
    for (size_t keep = n; keep > 2; keep--) {
        size_t head = keep / 2, tail = keep - head;
        std::string t = s.substr(0, cps[head]) + "..." + s.substr(cps[n - tail]);
        if (ImGui::CalcTextSize(t.c_str()).x <= maxW) return t;
    }
    return "...";
}

// ---------------------------------------------------------------- gallery view
void FileBrowser::DrawGrid(std::string& navigateTo, bool& confirmed) {
    bool mobile = ui::M().mobile;
    ImGuiIO& io = ImGui::GetIO();
    if (mobile) ui::TouchScroll();
    float gap = ImGui::GetStyle().ItemSpacing.x * 0.6f;
    float avail = ImGui::GetContentRegionAvail().x;
    float want = (float)prefs.tile * ui::M().scale;
    int cols = std::max(mobile ? 3 : 2, (int)floorf((avail + gap) / (want + gap)));
    float cell = floorf((avail - gap * (float)(cols - 1)) / (float)cols);
    float labelH = ImGui::GetTextLineHeight();
    float rowH = cell + labelH + gap * 2;
    int n = (int)entries_.size();
    int rows = (n + cols - 1) / cols;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float rounding = ImGui::GetFontSize() * 0.3f;
    bool multi = mode_ == Mode::OpenMulti;
    ImGuiListClipper clip;
    clip.Begin(rows, rowH);
    while (clip.Step()) {
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
            for (int c = 0; c < cols; c++) {
                int i = r * cols + c;
                if (i >= n) break;
                const DirEntry& e = entries_[(size_t)i];
                std::string full = JoinPath(dir_, e.name);
                bool sel = selected_.count(full) > 0;
                if (c) ImGui::SameLine(0, gap);
                ImGui::PushID(i);
                ImVec2 p0 = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton("##tile", ImVec2(cell, cell + labelH + gap), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
                bool hovered = ImGui::IsItemHovered();
                bool clicked = ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(0) && hovered && !longPressHandled_ &&
                               fabsf(ImGui::GetMouseDragDelta(0, 0).y) < 6;
                if (ImGui::IsItemDeactivated()) longPressHandled_ = false;
                bool dbl = hovered && ImGui::IsMouseDoubleClicked(0);
                bool isImg = !e.isDir && ImageCache::IsImagePath(e.name);
                // long press / right click: folders get selected, pictures show a big preview
                bool longPress = mobile && ImGui::IsItemActive() && io.MouseDownDuration[0] > 0.55f && ImGui::GetMouseDragDelta(0).y == 0 &&
                                 !longPressHandled_;
                if (longPress || (hovered && ImGui::IsMouseClicked(1))) {
                    if (isImg) { previewPath_ = full; openPreview_ = true; }
                    else if (multi) { if (sel) selected_.erase(full); else selected_.insert(full); }
                    if (longPress) longPressHandled_ = true;
                }
                if (dbl) Click(i, confirmed, navigateTo, true);
                else if (clicked) Click(i, confirmed, navigateTo, false);
                sel = selected_.count(full) > 0;

                ImVec2 t0 = p0, t1(p0.x + cell, p0.y + cell);
                dl->AddRectFilled(t0, t1, ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), rounding);
                if (isImg) {
                    const ImageTex& t = Images().Get(full, ImageCache::Size::Tile);
                    if (t.state == ImageTex::Ready) {
                        ImVec2 uv0(0, 0), uv1(1, 1);
                        if (t.w > t.h) { float f = (float)t.h / (float)t.w; uv0.x = (1 - f) * 0.5f; uv1.x = 1 - uv0.x; }
                        else { float f = (float)t.w / (float)t.h; uv0.y = (1 - f) * 0.5f; uv1.y = 1 - uv0.y; }
                        dl->AddImageRounded(t.Id(), t0, t1, uv0, uv1, IM_COL32_WHITE, rounding);
                    } else {
                        ui::DrawIcon(dl, ui::Icon::Image, ImVec2(p0.x + cell * 0.5f, p0.y + cell * 0.5f), cell * 0.3f,
                                     t.state == ImageTex::Failed ? ui::ColErr() : ui::ColMuted());
                    }
                } else {
                    ui::DrawIcon(dl, e.isDir ? ui::Icon::Folder : ui::Icon::File, ImVec2(p0.x + cell * 0.5f, p0.y + cell * 0.45f), cell * 0.38f,
                                 e.isDir ? ui::ColWarn() : ui::ColMuted());
                    if (!e.isDir) {
                        size_t dot = e.name.find_last_of('.');
                        std::string ext = dot == std::string::npos ? "" : e.name.substr(dot + 1);
                        if (ext.size() > 6) ext.resize(6);
                        for (auto& ch : ext) ch = (char)toupper((unsigned char)ch);
                        ImVec2 es = ImGui::CalcTextSize(ext.c_str());
                        dl->AddText(ImVec2(p0.x + (cell - es.x) * 0.5f, p0.y + cell * 0.72f), ImGui::GetColorU32(ImGuiCol_Text), ext.c_str());
                    }
                }
                // selection marker
                if (multi && !e.isDir) {
                    float rr = std::max(8.0f, cell * 0.09f);
                    ImVec2 cc(t1.x - rr - 5, t0.y + rr + 5);
                    if (sel) {
                        dl->AddCircleFilled(cc, rr, ui::ColAccent());
                        ui::DrawIcon(dl, ui::Icon::Check, cc, rr * 1.2f, IM_COL32_WHITE);
                    } else if (mobile || hovered) {
                        dl->AddCircleFilled(cc, rr, IM_COL32(0, 0, 0, 90));
                        dl->AddCircle(cc, rr, IM_COL32(255, 255, 255, 220), 0, 2.0f);
                    }
                }
                if (sel) dl->AddRect(t0, t1, ui::ColAccent(), rounding, 0, 3.0f);
                // label
                ImVec2 lp(p0.x, t1.y + gap * 0.5f);
                std::string label = MiddleEllipsis(e.name, cell);
                dl->PushClipRect(lp, ImVec2(lp.x + cell, lp.y + labelH), true);
                dl->AddText(lp, ImGui::GetColorU32(sel ? ImGuiCol_Text : ImGuiCol_TextDisabled), label.c_str());
                dl->PopClipRect();
                if (hovered && !mobile && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                    ImGui::SetTooltip("%s\n%s%s%s", e.name.c_str(), e.isDir ? "folder" : FormatBytes((double)e.size).c_str(),
                                      e.mtime ? "  -  " : "", e.mtime ? FormatTime(e.mtime * 1000).c_str() : "");
                ImGui::PopID();
            }
        }
    }
    clip.End();
}

// ---------------------------------------------------------------- main
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
        ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.75f, vp->WorkSize.y * 0.8f), ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    }
    bool confirmed = false;
    bool keepOpen = true;
    ImGuiWindowFlags wf = mobile ? (ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar) : 0;
    if (!ImGui::BeginPopupModal(popupId, mobile ? nullptr : &keepOpen, wf)) {
        open_ = false;
        return false;
    }
    Prefs before = prefs;
    ImGui::TextUnformatted(title_.c_str());
    if (mobile) ImGui::Separator();

    // ---- toolbar: navigation
    float bs = mobile ? ui::M().rowH * 0.85f : 0;
    if (ui::IconButton("up", ui::Icon::Up, T("Parent folder"), bs)) {
        std::string parent = DirName(dir_);
        if (!parent.empty() && parent != dir_) Navigate(parent);
    }
    ImGui::SameLine();
    if (ui::IconButton("refresh", ui::Icon::Retry, T("Refresh"), bs)) Refresh();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##path", &pathBuf_, ImGuiInputTextFlags_EnterReturnsTrue)) Navigate(pathBuf_);

    // ---- toolbar: view
    if (mode_ != Mode::SelectDir) {
        auto toggle = [&](const char* id, ui::Icon icon, const char* tip, bool on) {
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            bool r = ui::IconButton(id, icon, tip, bs);
            if (on) ImGui::PopStyleColor();
            return r;
        };
        if (toggle("vlist", ui::Icon::List, T("List"), !prefs.grid)) prefs.grid = false;
        ImGui::SameLine();
        if (toggle("vgrid", ui::Icon::Grid, T("Gallery (thumbnails)"), prefs.grid)) prefs.grid = true;
        ImGui::SameLine();
        if (mode_ == Mode::OpenMulti) {
            if (ImGui::Checkbox(T("Pictures only"), &prefs.picturesOnly)) Refresh();
            ImGui::SameLine();
        }
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * (mobile ? 7.5f : 8.5f));
        if (ImGui::Combo("##sort", &prefs.sort, TItems("Name\0Newest first\0Largest first\0"))) Refresh();
        if (prefs.grid && !mobile) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
            ImGui::SliderInt("##tile", &prefs.tile, 80, 260, T("Size %d"));
        }
        if (mode_ == Mode::OpenMulti && !mobile) {
            ImGui::SameLine();
            if (ImGui::Button(T("Select all files"))) {
                for (auto& e : entries_)
                    if (!e.isDir) selected_.insert(JoinPath(dir_, e.name));
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(selected_.empty());
            if (ImGui::Button(T("Clear"))) selected_.clear();
            ImGui::EndDisabled();
        }
    }

    float footerH = (mobile ? ui::M().rowH : ImGui::GetFrameHeight()) * (mode_ == Mode::SaveFile ? 2.4f : 1.3f) +
                    ImGui::GetStyle().ItemSpacing.y * 3 + (mobile && mode_ == Mode::OpenMulti ? ui::M().rowH * 0.9f : 0);
    float placesW = mobile ? 0 : ImGui::GetFontSize() * 11;

    if (mobile) {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##places", T("Go to..."))) {
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
    if (resetScroll_) { ImGui::SetScrollY(0); resetScroll_ = false; }
    if (!error_.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", error_.c_str());
    std::string navigateTo;
    if (entries_.empty()) ImGui::TextDisabled(prefs.picturesOnly ? T("No pictures or folders here.") : T("This folder is empty."));
    else if (prefs.grid && mode_ != Mode::SelectDir) DrawGrid(navigateTo, confirmed);
    else DrawList(navigateTo, confirmed);
    ImGui::EndChild();
    if (!navigateTo.empty()) Navigate(navigateTo);

    // ---- footer
    if (mobile && mode_ == Mode::OpenMulti) {
        float w3 = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3;
        if (ImGui::Button(T("Select all"), ImVec2(w3, ui::M().rowH * 0.8f)))
            for (auto& e : entries_)
                if (!e.isDir) selected_.insert(JoinPath(dir_, e.name));
        ImGui::SameLine();
        ImGui::BeginDisabled(selected_.empty());
        if (ImGui::Button(T("Clear"), ImVec2(w3, ui::M().rowH * 0.8f))) selected_.clear();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        if (!selected_.empty()) ImGui::TextDisabled("%d - %s", (int)selected_.size(), FormatBytes((double)SelectedBytes()).c_str());
        else ImGui::TextDisabled(T("Tap to select"));
    }
    if (mode_ == Mode::SaveFile) {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##name", T("File name"), &nameBuf_);
    }
    if (mode_ == Mode::SelectDir) {
        ImGui::SetNextItemWidth(mobile ? -1 : ImGui::GetContentRegionAvail().x * 0.5f);
        ImGui::InputTextWithHint("##newfolder", T("New folder name"), &newFolderBuf_);
        if (!mobile) ImGui::SameLine();
        if (ImGui::Button(T("Create folder")) && !newFolderBuf_.empty()) {
            std::string nd = JoinPath(dir_, SanitizeRelativePath(newFolderBuf_));
            if (MakeDirs(nd)) { newFolderBuf_.clear(); Navigate(nd); }
        }
    }
    const char* okLabel = mode_ == Mode::SaveFile ? T("Save") : mode_ == Mode::SelectDir ? T("Use this folder") : T("Open");
    std::string okText = okLabel;
    if (mode_ == Mode::OpenMulti) okText = selected_.size() > 1 ? TF("Send %d items", (int)selected_.size()) : std::string(T("Send"));
    float bw = mobile ? (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f : ImGui::GetFontSize() * 9;
    float bh = mobile ? ui::M().rowH : 0;
    bool canOk = mode_ == Mode::SelectDir || (mode_ == Mode::SaveFile && !nameBuf_.empty()) || !selected_.empty();
    if (!mobile) {
        if (mode_ == Mode::OpenMulti && !selected_.empty()) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled(T("%d selected - %s"), (int)selected_.size(), FormatBytes((double)SelectedBytes()).c_str());
            ImGui::SameLine();
        }
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - bw * 2 - ImGui::GetStyle().ItemSpacing.x);
    }
    ImGui::BeginDisabled(!canOk);
    if (ImGui::Button(okText.c_str(), ImVec2(bw, bh))) confirmed = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(T("Cancel"), ImVec2(bw, bh)) || (!mobile && ImGui::IsKeyPressed(ImGuiKey_Escape) && !openPreview_)) keepOpen = false;
    if (ImGui::IsKeyPressed(ImGuiKey_AppBack) && !openPreview_) keepOpen = false;

    // ---- big preview (long press / right click on a picture)
    if (openPreview_) {
        ImGui::OpenPopup("##preview");
        openPreview_ = false;
    }
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopup("##preview", ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize)) {
        const ImageTex& full = Images().Get(previewPath_, ImageCache::Size::Thumb);
        float maxW = vp->WorkSize.x * 0.9f, maxH = vp->WorkSize.y * 0.75f;
        if (full.state == ImageTex::Ready) {
            float s = std::min(maxW / (float)full.w, maxH / (float)full.h);
            ImGui::Image(ImTextureRef(full.Id()), ImVec2((float)full.w * s, (float)full.h * s));
        } else {
            ImGui::Dummy(ImVec2(maxW * 0.5f, maxH * 0.5f));
        }
        ImGui::TextUnformatted(BaseName(previewPath_).c_str());
        bool sel = selected_.count(previewPath_) > 0;
        if (mode_ == Mode::OpenMulti && ImGui::Button(sel ? T("Unselect") : T("Select"), ImVec2(0, bh))) {
            if (sel) selected_.erase(previewPath_); else selected_.insert(previewPath_);
            ImGui::CloseCurrentPopup();
        }
        if (mode_ == Mode::OpenMulti) ImGui::SameLine();
        if (ImGui::Button(T("Close"), ImVec2(0, bh)) || ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_AppBack))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

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
    if (prefs != before) prefsChanged = true;
    ImGui::EndPopup();
    return confirmed;
}
