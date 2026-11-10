// LunaShare - comics: cbz / cbr / cb7 / cbt (and any archive that is mostly pictures, through Read). Opening one
// unpacks its pages into the cache (the last few comics stay there, so reopening is instant) and shows them in the
// picture viewer, in order, at the page where you stopped; manga can be read right to left. The Comics view of the
// Archives tab shows them as covers with how far each one is read. Places kept in comic_positions.json.
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "archives.h"
#include "hash.h"
#include "i18n.h"
#include "image_cache.h"
#include "imgui.h"
#include "platform.h"
#include "ui.h"
#include "ui_widgets.h"

using namespace ls;
using json = nlohmann::json;
using ui::Icon;
namespace fs = std::filesystem;

namespace {
// ---- where each comic was left
struct Place {
    int page = 0, pages = 0;
    bool rtl = false;
    int64_t at = 0;
};
std::mutex g_pm;
std::map<std::string, Place> g_places;
std::string g_placesPath;
bool g_placesDirty = false;

void LoadPlaces(const std::string& cfg) {
    std::lock_guard<std::mutex> l(g_pm);
    if (!g_placesPath.empty()) return;
    g_placesPath = JoinPath(cfg, "comic_positions.json");
    json j;
    if (!LoadJsonFile(g_placesPath, j) || !j.is_object()) return;
    for (auto& [k, v] : j.items())
        if (v.is_object()) g_places[k] = {v.value("page", 0), v.value("pages", 0), v.value("rtl", false), v.value("at", (int64_t)0)};
}
void SavePlaces() {
    std::lock_guard<std::mutex> l(g_pm);
    if (!g_placesDirty || g_placesPath.empty()) return;
    json j = json::object();
    for (auto& [k, p] : g_places) j[k] = {{"page", p.page}, {"pages", p.pages}, {"rtl", p.rtl}, {"at", p.at}};
    SaveJsonFile(g_placesPath, j);
    g_placesDirty = false;
}
Place GetPlace(const std::string& k) {
    std::lock_guard<std::mutex> l(g_pm);
    auto it = g_places.find(k);
    return it == g_places.end() ? Place() : it->second;
}

std::string Hash16(const std::string& s) { return Sha256Hex(s.data(), s.size()).substr(0, 16); }

// ---- the cache: a folder per comic (by its path, size and date), the last few kept
std::string CacheRoot(const std::string& cfg) { return JoinPath(cfg, "cache/comics"); }
std::string CacheDir(const std::string& cfg, const std::string& path) {
    std::string key = path + "|" + std::to_string(FileSize(path)) + "|" + std::to_string(FileMTime(path));
    return JoinPath(CacheRoot(cfg), Hash16(key));
}
void Prune(const std::string& cfg, const std::string& keep) {
    std::vector<std::pair<int64_t, std::string>> dirs;
    for (auto& e : ListDir(CacheRoot(cfg)))
        if (e.isDir && e.name != "covers") dirs.push_back({e.mtime, JoinPath(CacheRoot(cfg), e.name)});
    std::sort(dirs.begin(), dirs.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (size_t i = 6; i < dirs.size(); i++)
        if (dirs[i].second != keep) {
            std::error_code ec;
            fs::remove_all(fs::u8path(dirs[i].second), ec);
        }
}

// the pages: the pictures inside, in reading order (names compared like people do: 2 before 10)
std::vector<std::string> Pages(const arc::Listing& l) {
    std::vector<std::string> v;
    for (auto& e : l.entries) {
        if (e.dir || !ImageCache::IsImagePath(e.path)) continue;
        std::string b = BaseName(e.path);
        if (!b.empty() && b[0] == '.') continue;  // (macOS leftovers: ._page.jpg)
        if (e.path.find("__MACOSX") != std::string::npos) continue;
        v.push_back(e.path);
    }
    std::sort(v.begin(), v.end(), [](const std::string& a, const std::string& b) {
        std::string da = DirName(a), db = DirName(b);
        if (a.find('/') == std::string::npos) da.clear();
        if (b.find('/') == std::string::npos) db.clear();
        return da != db ? NaturalLess(da, db) : NaturalLess(BaseName(a), BaseName(b));
    });
    return v;
}

// ---- opening (on a thread): unpack the pages, then the viewer
struct Opening {
    std::thread th;
    std::atomic<bool> running{false}, ready{false}, cancel{false};
    std::atomic<int> done{0}, total{0};
    std::mutex m;
    std::string path, dir, error;
    std::vector<std::string> pages;  // (full paths, in order)
    ~Opening() {
        cancel = true;
        if (th.joinable()) th.join();
    }
} g_open;

// ---- covers (on a thread): the first page of each comic on the shelf, small, in the cache
struct Covers {
    std::thread th;
    std::mutex m;
    std::set<std::string> wanted, failed;
    std::map<std::string, std::string> made;  // comic -> picture
    std::atomic<bool> busy{false};
    ~Covers() {
        if (th.joinable()) th.join();
    }
} g_covers;
}  // namespace

bool App::ComicOpening() const { return g_open.running; }

void App::OpenComic(const std::string& path, const std::string& password) {
    LoadPlaces(eng.ConfigPath());
    if (g_open.running) return;
    if (g_open.th.joinable()) g_open.th.join();
    std::string cfg = eng.ConfigPath();
    g_open.cancel = false;
    g_open.ready = false;
    g_open.done = 0;
    g_open.total = 0;
    {
        std::lock_guard<std::mutex> l(g_open.m);
        g_open.path = path;
        g_open.error.clear();
        g_open.pages.clear();
    }
    g_open.running = true;
    g_open.th = std::thread([path, password, cfg] {
        std::string dir = CacheDir(cfg, path), err;
        std::vector<std::string> pages;
        arc::Listing l;
        bool ok = arc::List(path, l, password);
        std::vector<std::string> rel = Pages(l);
        if (!ok || rel.empty()) err = l.needPassword ? "needs a password" : !l.error.empty() ? l.error : "no pictures inside";
        if (err.empty() && FileSize(JoinPath(dir, ".complete")) < 0) {  // (not unpacked yet: do it)
            g_open.total = (int)rel.size();
            std::error_code ec;
            fs::remove_all(fs::u8path(dir), ec);
            arc::Progress pr = [&](int64_t d, int64_t t, const std::string&) {
                g_open.done = t > 0 ? (int)((double)d / (double)t * rel.size()) : 0;
                return !g_open.cancel.load();
            };
            if (arc::Extract(path, dir, rel, password, pr, err)) {
                if (FILE* f = OpenFile(JoinPath(dir, ".complete"), "wb")) fclose(f);
            }
        } else if (err.empty()) {
            // (touched: the most recent stays when the cache is pruned)
            if (FILE* f = OpenFile(JoinPath(dir, ".complete"), "wb")) fclose(f);
            std::error_code ec;
            fs::last_write_time(fs::u8path(dir), fs::file_time_type::clock::now(), ec);
        }
        if (err.empty()) {
            for (auto& r : rel) pages.push_back(JoinPath(dir, arc::SafeRelPath(r)));
            Prune(cfg, dir);
        }
        std::lock_guard<std::mutex> lk(g_open.m);
        g_open.dir = dir;
        g_open.error = err;
        g_open.pages = pages;
        g_open.ready = true;
        g_open.running = false;
    });
}

// every frame the viewer shows a comic (and when it starts / stops): the page read is kept
void App::ComicTick() {
    if (viewer.comic.empty()) return;
    std::lock_guard<std::mutex> l(g_pm);
    Place& p = g_places[viewer.comic];
    if (p.page != viewer.index || p.pages != (int)viewer.paths.size() || p.rtl != viewer.rtl) {
        p.page = viewer.index;
        p.pages = (int)viewer.paths.size();
        p.rtl = viewer.rtl;
        p.at = NowMs();
        g_placesDirty = true;
    }
}

// every frame (wherever the comic was opened from): one that finished opening goes to the viewer, at its page
void App::ComicFrame() {
    if (g_open.running) busy = true;
    {
        std::lock_guard<std::mutex> l(g_open.m);
        if (g_open.ready) {
            g_open.ready = false;
            if (!g_open.error.empty()) {
                toasts.push_back({EventType::Error, T("Comics"), TF("Can't open %s: %s", BaseName(g_open.path).c_str(), g_open.error.c_str()), "", 0, 5.0f});
            } else {
                Place pl = GetPlace(g_open.path);
                std::vector<std::string> caps(g_open.pages.size(), BaseName(g_open.path));
                OpenViewerList(g_open.pages, pl.pages == (int)g_open.pages.size() ? pl.page : 0, caps);
                viewer.comic = g_open.path;
                viewer.rtl = pl.rtl;
                ComicTick();
                std::lock_guard<std::mutex> pl2(g_pm);  // (read now: first on the shelf)
                g_places[g_open.path].at = NowMs();
                g_placesDirty = true;
            }
        }
    }
    static int64_t lastSave = 0;
    if (SteadyMs() - lastSave > 5000 || (!viewer.open && g_placesDirty)) {
        lastSave = SteadyMs();
        SavePlaces();
    }
}

void App::DrawComicShelf(const std::vector<std::string>& paths, bool scanning) {
    bool mobile = ui::M().mobile;
    LoadPlaces(eng.ConfigPath());
    if (g_open.running) {
        int d = g_open.done, t = g_open.total;
        ImGui::TextUnformatted(TF("Opening %s", BaseName(g_open.path).c_str()).c_str());
        ImGui::SameLine();
        ImGui::ProgressBar(t > 0 ? (float)d / (float)t : -1.0f * (float)ImGui::GetTime(), ImVec2(ImGui::GetFontSize() * 12, 0),
                           t > 0 ? TF("page %d / %d", d, t).c_str() : "");
        ImGui::SameLine();
        if (ImGui::SmallButton(T("Stop"))) g_open.cancel = true;
    }
    // the shelf: read lately first, then by name
    std::vector<std::string> list = paths;
    std::stable_sort(list.begin(), list.end(), [](const std::string& a, const std::string& b) {
        int64_t ta = GetPlace(a).at, tb = GetPlace(b).at;
        return ta != tb ? ta > tb : NaturalLess(BaseName(a), BaseName(b));
    });
    // covers wanted for those not made yet (one thread works through them)
    {
        std::lock_guard<std::mutex> l(g_covers.m);
        for (auto& p : list)
            if (!g_covers.made.count(p) && !g_covers.failed.count(p)) g_covers.wanted.insert(p);
    }
    if (!g_covers.busy) {
        bool any;
        {
            std::lock_guard<std::mutex> l(g_covers.m);
            any = !g_covers.wanted.empty();
        }
        if (any) {
            if (g_covers.th.joinable()) g_covers.th.join();
            g_covers.busy = true;
            std::string covers = JoinPath(CacheRoot(eng.ConfigPath()), "covers");
            g_covers.th = std::thread([covers] {
                for (;;) {
                    std::string p;
                    {
                        std::lock_guard<std::mutex> l(g_covers.m);
                        if (g_covers.wanted.empty()) break;
                        p = *g_covers.wanted.begin();
                        g_covers.wanted.erase(g_covers.wanted.begin());
                    }
                    // the first page, unpacked once next to the others ("<hash>.<ext>")
                    std::string key = Hash16(p + "|" + std::to_string(FileSize(p)));
                    std::string found;
                    for (auto& e : ListDir(covers))
                        if (e.name.compare(0, key.size(), key) == 0) found = JoinPath(covers, e.name);
                    if (found.empty()) {
                        arc::Listing l;
                        arc::List(p, l);
                        auto pages = Pages(l);
                        std::string err;
                        std::vector<std::string> got;
                        if (!pages.empty()) {
                            std::string tmp = JoinPath(covers, key + ".tmp");
                            if (arc::Extract(p, tmp, {pages[0]}, "", nullptr, err, &got) && !got.empty()) {
                                std::string ext = got[0].substr(got[0].find_last_of('.'));
                                found = JoinPath(covers, key + ext);
                                RenameFile(got[0], found);
                            }
                            std::error_code ec;
                            fs::remove_all(fs::u8path(tmp), ec);
                        }
                    }
                    std::lock_guard<std::mutex> l(g_covers.m);
                    if (found.empty()) g_covers.failed.insert(p);
                    else g_covers.made[p] = found;
                }
                g_covers.busy = false;
            });
        }
    }
    ImGui::BeginChild("comics");
    if (mobile) ui::TouchScroll();
    if (list.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", scanning ? T("looking through the folders...") : T("No comics (cbz, cbr, cb7, cbt) in these folders."));
        ImGui::PopTextWrapPos();
    }
    float avail = ImGui::GetContentRegionAvail().x;
    float sp = ImGui::GetStyle().ItemSpacing.x;
    int cols = std::max(2, (int)((avail + sp) / (ImGui::GetFontSize() * (mobile ? 7.0f : 9.0f) + sp)));
    float cw = (avail - sp * (cols - 1)) / cols, ch = cw * 1.45f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (size_t i = 0; i < list.size(); i++) {
        const std::string& p = list[i];
        if (i % cols) ImGui::SameLine(0, sp);
        ImGui::PushID((int)i);
        ImGui::BeginGroup();
        ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + cw, a.y + ch);
        if (ImGui::InvisibleButton("##c", ImVec2(cw, ch)) && !g_open.running) OpenComic(p);
        bool hov = ImGui::IsItemHovered();
        if (ImGui::IsRectVisible(a, b)) {
            dl->AddRectFilled(a, b, ImGui::GetColorU32(hov ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), 4);
            std::string cover;
            {
                std::lock_guard<std::mutex> l(g_covers.m);
                auto it = g_covers.made.find(p);
                if (it != g_covers.made.end()) cover = it->second;
            }
            const ImageTex* t = cover.empty() ? nullptr : &Images().Get(cover, ImageCache::Size::Thumb);
            if (t && t->state == ImageTex::Ready) {
                float s = std::min(cw / t->w, ch / t->h);
                float w = t->w * s, hh = t->h * s;
                ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
                dl->AddImage(t->Id(), ImVec2(c.x - w * 0.5f, c.y - hh * 0.5f), ImVec2(c.x + w * 0.5f, c.y + hh * 0.5f));
            } else {
                ui::DrawIcon(dl, Icon::Book, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), cw * 0.35f, ui::ColMuted());
            }
            Place pl = GetPlace(p);
            if (pl.pages > 0) {  // how far: a bar at the bottom; done = a tick
                float fr = std::min(1.0f, (float)(pl.page + 1) / (float)pl.pages);
                dl->AddRectFilled(ImVec2(a.x, b.y - 5), b, IM_COL32(0, 0, 0, 150));
                dl->AddRectFilled(ImVec2(a.x, b.y - 5), ImVec2(a.x + cw * fr, b.y), IM_COL32(230, 70, 70, 255));
                if (pl.page + 1 >= pl.pages) {
                    float r = cw * 0.1f;
                    ImVec2 cc(a.x + r + 5, a.y + r + 5);
                    dl->AddCircleFilled(cc, r, IM_COL32(40, 150, 90, 235));
                    ui::DrawIcon(dl, Icon::Check, cc, r * 1.6f, IM_COL32_WHITE);
                }
            }
            if (g_open.running && g_open.path == p) dl->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_SliderGrabActive), 4.0f, 0, 3.0f);
        }
        if (hov && !mobile) {
            Place pl = GetPlace(p);
            ImGui::SetTooltip("%s%s", p.c_str(), pl.pages ? ("\n" + TF("page %d / %d", pl.page + 1, pl.pages)).c_str() : "");
        }
        ImVec2 tp = ImGui::GetCursorScreenPos();
        ImVec4 clip(tp.x, tp.y, tp.x + cw, tp.y + ImGui::GetTextLineHeight() * 2.1f);
        std::string name = BaseName(p);
        size_t dot = name.find_last_of('.');
        if (dot != std::string::npos) name.resize(dot);
        dl->AddText(nullptr, 0, tp, ImGui::GetColorU32(ImGuiCol_Text), name.c_str(), nullptr, cw, &clip);
        ImGui::Dummy(ImVec2(cw, ImGui::GetTextLineHeight() * 2.1f));
        ImGui::EndGroup();
        ImGui::PopID();
    }
    ImGui::EndChild();
}
