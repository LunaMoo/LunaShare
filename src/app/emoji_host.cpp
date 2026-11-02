// SPDX-License-Identifier: GPL-3.0-only
#include "emoji_host.h"
#include "i18n.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "bluetooth.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"
#include "platform.h"
#include "ui_widgets.h"

#include "emoji_props.inc"

using namespace ls;

EmojiHost& Emoji() {
    static EmojiHost h;
    return h;
}

namespace {

constexpr uint32_t kBase = 0xF0000, kMax = 0xFFFFD;

template <size_t N>
bool In(const uint32_t (&r)[N][2], uint32_t c) {
    size_t lo = 0, hi = N;
    while (lo < hi) {
        size_t m = (lo + hi) / 2;
        if (c < r[m][0]) hi = m;
        else if (c > r[m][1]) lo = m + 1;
        else return true;
    }
    return false;
}
bool IsRI(uint32_t c) { return c >= 0x1F1E6 && c <= 0x1F1FF; }
bool IsMod(uint32_t c) { return c >= 0x1F3FB && c <= 0x1F3FF; }
bool IsTag(uint32_t c) { return c >= 0xE0020 && c <= 0xE007E; }
bool IsKeycapBase(uint32_t c) { return (c >= '0' && c <= '9') || c == '#' || c == '*'; }

// decodes one code point at s[i], returns its byte length (invalid bytes count as one U+FFFD)
size_t Decode(const std::string& s, size_t i, uint32_t* out) {
    auto b = (unsigned char)s[i];
    size_t n = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 0;
    if (n == 0 || i + n > s.size()) {
        *out = 0xFFFD;
        return 1;
    }
    uint32_t c = n == 1 ? b : n == 2 ? b & 0x1F : n == 3 ? b & 0x0F : b & 0x07;
    for (size_t k = 1; k < n; k++) {
        auto x = (unsigned char)s[i + k];
        if ((x & 0xC0) != 0x80) {
            *out = 0xFFFD;
            return 1;
        }
        c = c << 6 | (x & 0x3F);
    }
    *out = c;
    return n;
}

void Append(std::string& o, uint32_t c) {
    if (c < 0x80) o += (char)c;
    else if (c < 0x800) { o += (char)(0xC0 | c >> 6); o += (char)(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { o += (char)(0xE0 | c >> 12); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
    else { o += (char)(0xF0 | c >> 18); o += (char)(0x80 | ((c >> 12) & 0x3F)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
}

bool MayHaveEmoji(const std::string& s) {
    for (unsigned char c : s)
        if (c >= 0xC2) return true;  // anything outside ASCII (emoji start at U+00A9); keycaps need U+20E3
    return false;
}

}  // namespace

// ---------------------------------------------------------------- text
std::vector<EmojiHost::Segment> EmojiHost::Segments(const std::string& s) const {
    struct Cp {
        uint32_t c;
        size_t at;
    };
    std::vector<Cp> cps;
    for (size_t i = 0; i < s.size();) {
        uint32_t c;
        size_t n = Decode(s, i, &c);
        cps.push_back({c, i});
        i += n;
    }
    auto at = [&](size_t k) { return k < cps.size() ? cps[k].at : s.size(); };
    auto cp = [&](size_t k) { return k < cps.size() ? cps[k].c : 0u; };
    std::vector<Segment> out;
    auto add = [&](size_t a, size_t b, bool emoji) {
        if (!emoji && !out.empty() && !out.back().emoji) out.back().len = at(b) - out.back().start;
        else out.push_back({at(a), at(b) - at(a), emoji});
    };
    size_t n = cps.size();
    for (size_t i = 0; i < n;) {
        uint32_t c = cps[i].c;
        size_t j = i + 1;
        bool emoji = false;
        if (IsRI(c) && IsRI(cp(i + 1))) {
            j = i + 2;
            emoji = true;
        } else if (IsKeycapBase(c) && (cp(i + 1) == 0x20E3 || (cp(i + 1) == 0xFE0F && cp(i + 2) == 0x20E3))) {
            j = cp(i + 1) == 0x20E3 ? i + 2 : i + 3;
            emoji = true;
        } else if (c >= 0xA9 && (In(kExtPict, c) || In(kEmojiPresentation, c))) {
            emoji = In(kEmojiPresentation, c);
            if (cp(j) == 0xFE0F) emoji = true, j++;
            else if (cp(j) == 0xFE0E) emoji = false;
            if (IsMod(cp(j))) emoji = true, j++;
            if (IsTag(cp(j))) {
                while (IsTag(cp(j))) j++;
                if (cp(j) == 0xE007F) j++;
                emoji = true;
            }
            while (emoji && cp(j) == 0x200D && (In(kExtPict, cp(j + 1)) || In(kEmoji, cp(j + 1)))) {
                j += 2;
                if (cp(j) == 0xFE0F) j++;
                if (IsMod(cp(j))) j++;
            }
            if (!emoji) j = i + 1;
        }
        add(i, j, emoji);
        i = j;
    }
    return out;
}

uint32_t EmojiHost::IdFor(const std::string& seq) {
    auto it = ids_.find(seq);
    if (it != ids_.end()) return it->second;
    if (kBase + seqs_.size() > kMax) return 0;
    uint32_t id = kBase + (uint32_t)seqs_.size();
    seqs_.push_back(seq);
    ids_[seq] = id;
    return id;
}

std::string EmojiHost::Glyph(const std::string& emoji) {
    if (!mod_) return emoji;
    std::string o;
    Append(o, IdFor(emoji));
    return o;
}

const std::string& EmojiHost::Display(const std::string& text) {
    if (!mod_ || !MayHaveEmoji(text)) return text;
    auto it = cache_.find(text);
    if (it != cache_.end()) return it->second;
    if (cache_.size() > 4000) cache_.clear();
    std::string o;
    o.reserve(text.size());
    bool changed = false;
    for (auto& sg : Segments(text)) {
        if (sg.emoji) {
            if (uint32_t id = IdFor(text.substr(sg.start, sg.len))) {
                Append(o, id);
                changed = true;
                continue;
            }
        }
        // drop leftover presentation selectors: they have no glyph of their own
        for (size_t i = sg.start; i < sg.start + sg.len;) {
            uint32_t c;
            size_t n = Decode(text, i, &c);
            if (c == 0xFE0F || c == 0xFE0E) changed = true;
            else o.append(text, i, n);
            i += n;
        }
    }
    if (!changed) o = text;
    return cache_[text] = std::move(o);
}

std::string EmojiHost::Restore(const std::string& s) const {
    if (seqs_.empty()) return s;
    std::string o;
    o.reserve(s.size() + 16);
    for (size_t i = 0; i < s.size();) {
        uint32_t c;
        size_t n = Decode(s, i, &c);
        if (c >= kBase && c < kBase + seqs_.size()) o += seqs_[c - kBase];
        else o.append(s, i, n);
        i += n;
    }
    return o;
}

int EmojiHost::EmojiOnly(const std::string& text) {
    if (!mod_ || !MayHaveEmoji(text) || text.size() > 120) return 0;
    int count = 0;
    for (auto& sg : Segments(text)) {
        if (sg.emoji) count++;
        else
            for (size_t i = sg.start; i < sg.start + sg.len; i++)
                if (!isspace((unsigned char)text[i])) return 0;
    }
    return count <= 3 ? count : 0;
}

int EmojiHost::InputCallback(ImGuiInputTextCallbackData* d) {
    EmojiHost& e = Emoji();
    if (d->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
        e.inputCursor_ = d->CursorPos;
    } else if (d->EventFlag == ImGuiInputTextFlags_CallbackEdit && e.mod_) {
        // typed / pasted emoji (phone keyboards send whole sequences) become single glyphs
        std::string buf(d->Buf, (size_t)d->BufTextLen);
        const std::string& disp = e.Display(buf);
        if (&disp != &buf && disp != buf) {
            std::string before = e.Display(buf.substr(0, (size_t)d->CursorPos));
            std::string text = disp;  // copy: Display()'s cache may change below
            d->DeleteChars(0, d->BufTextLen);
            d->InsertChars(0, text.c_str());
            d->CursorPos = d->SelectionStart = d->SelectionEnd = (int)std::min(before.size(), text.size());
        }
        e.inputCursor_ = d->CursorPos;
    }
    return 0;
}

// ---------------------------------------------------------------- font loader
struct EmojiFontLoader {
    static int GlyphIndex(ImFontAtlas*, ImFontConfig*, ImWchar c) {
        uint32_t u = (uint32_t)c;
        return u >= kBase && u < kBase + Emoji().seqs_.size() ? (int)(u - kBase + 1) : 0;
    }
    static bool Contains(ImFontAtlas*, ImFontConfig*, int g) { return g > 0 && (size_t)g <= Emoji().seqs_.size(); }
    static bool LoadGlyph(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void*, int g, ImFontGlyph* out, float* advOnly) {
        EmojiHost& e = Emoji();
        if (!e.mod_ || g <= 0 || (size_t)g > e.seqs_.size()) return false;
        float size = baked->Size, box = size * 1.12f, adv = box + size * 0.06f;
        if (advOnly) {
            *advOnly = adv;
            return true;
        }
        float density = src->RasterizerDensity * baked->RasterizerDensity;
        int px = std::max(4, (int)std::ceil(box * density));
        const uint8_t* rgba = e.mod_->render(e.seqs_[(size_t)g - 1].c_str(), px);
        out->AdvanceX = adv;
        if (!rgba) return false;  // the font has no picture: ImGui shows its fallback glyph
        ImFontAtlasRectId id = ImFontAtlasPackAddRect(atlas, px, px);
        if (id == ImFontAtlasRectId_Invalid) return false;
        ImTextureRect* r = ImFontAtlasPackGetRect(atlas, id);
        float s = 1.0f / density, x0 = size * 0.03f, y0 = std::floor((size - box) * 0.5f + size * 0.04f);
        out->X0 = x0;
        out->Y0 = y0;
        out->X1 = x0 + px * s;
        out->Y1 = y0 + px * s;
        out->Visible = true;
        out->Colored = true;
        out->PackId = id;
        ImFontAtlasBakedSetFontGlyphBitmap(atlas, baked, src, out, r, rgba, ImTextureFormat_RGBA32, px * 4);
        return true;
    }
    static const ImFontLoader* Get() {
        static ImFontLoader l;
        if (!l.Name) {
            l.Name = "LunaShare emoji";
            l.FontSrcInit = [](ImFontAtlas*, ImFontConfig*) { return true; };
            l.FontSrcGetGlyphIndexFromCodepoint = GlyphIndex;
            l.FontSrcContainsGlyph = Contains;
            l.FontBakedInit = [](ImFontAtlas*, ImFontConfig*, ImFontBaked*, void*) { return true; };
            l.FontBakedLoadGlyph = LoadGlyph;
        }
        return &l;
    }
};

void EmojiHost::AddToFont(ImFontAtlas* atlas) {
    if (!mod_ || atlas->Fonts.Size == 0) return;
    ImFontConfig cfg;
    cfg.MergeMode = true;
    cfg.FontLoader = EmojiFontLoader::Get();
    cfg.PixelSnapH = true;
    strncpy(cfg.Name, "Emoji", sizeof(cfg.Name) - 1);
    atlas->AddFont(&cfg);
}

static void (*g_prevSetClipboard)(ImGuiContext*, const char*) = nullptr;
void EmojiHost::InstallClipboardHook() {
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    if (!mod_ || pio.Platform_SetClipboardTextFn == nullptr || g_prevSetClipboard) return;
    g_prevSetClipboard = pio.Platform_SetClipboardTextFn;
    pio.Platform_SetClipboardTextFn = [](ImGuiContext* ctx, const char* text) {
        g_prevSetClipboard(ctx, Emoji().Restore(text ? text : "").c_str());
    };
}

// ---------------------------------------------------------------- module
void EmojiHost::Load() {
    std::vector<std::string> candidates;
#if defined(__ANDROID__)
    candidates.push_back("liblunashare_emoji.so");
#endif
    if (char* base = SDL_GetBasePath()) {
        std::string b = base;
        SDL_free(base);
#if defined(_WIN32)
        const char* names[] = {"lunashare_emoji.dll", "liblunashare_emoji.dll"};
#elif defined(__APPLE__)
        const char* names[] = {"liblunashare_emoji.dylib", "lunashare_emoji.dylib"};
#else
        const char* names[] = {"liblunashare_emoji.so", "lunashare_emoji.so"};
#endif
        for (auto& d : {b, JoinPath(b, "modules")})
            for (auto* n : names) candidates.push_back(JoinPath(d, n));
    }
    for (auto& c : candidates) {
#if !defined(__ANDROID__)
        if (!FileExists(c)) continue;
#endif
        void* lib = SDL_LoadObject(c.c_str());
        if (!lib) continue;
        auto fn = (LseGetModuleFn)SDL_LoadFunction(lib, "lunashare_emoji_module");
        const LseModule* m = fn ? fn() : nullptr;
        if (!m || m->api_version != LSE_API_VERSION) {
            loadInfo = c + ": incompatible module";
            SDL_UnloadObject(lib);
            continue;
        }
        host_ = LseHost{LSE_API_VERSION, this, [](void*, const char* msg) { SDL_Log("%s", msg); }, nullptr, nullptr};
        if (Bridge().javaVm) host_.java_vm = Bridge().javaVm();
        if (Bridge().emojiClass) host_.java_class = Bridge().emojiClass();
        if (!m->init(&host_)) {
            loadInfo = c + ": no colour emoji font on this system";
            SDL_UnloadObject(lib);
            continue;
        }
        lib_ = lib;
        mod_ = m;
        loadInfo = c;
        break;
    }
    if (!mod_ && loadInfo.empty()) loadInfo = "not installed";
}

// ---------------------------------------------------------------- picker
namespace {
std::string WithTone(const std::string& e, int tone) {
    if (tone <= 0) return e;
    uint32_t first;
    size_t n = Decode(e, 0, &first);
    std::string o = e.substr(0, n);
    Append(o, 0x1F3FA + (uint32_t)tone);
    size_t rest = n;
    uint32_t c;
    if (rest < e.size() && Decode(e, rest, &c) && c == 0xFE0F) rest += 3;
    return o + e.substr(rest);
}

const char* GroupIcon(const char* name) {
    static const struct { const char* name; const char* icon; } icons[] = {
        {"Smileys & Emotion", "\xF0\x9F\x98\x80"}, {"People & Body", "\xF0\x9F\x91\x8B"}, {"Animals & Nature", "\xF0\x9F\x90\xB6"},
        {"Food & Drink", "\xF0\x9F\x8D\x94"}, {"Travel & Places", "\xE2\x9C\x88\xEF\xB8\x8F"}, {"Activities", "\xE2\x9A\xBD"},
        {"Objects", "\xF0\x9F\x92\xA1"}, {"Symbols", "\xE2\x9D\xA4\xEF\xB8\x8F"}, {"Flags", "\xF0\x9F\x8F\x81"}};
    for (auto& i : icons)
        if (strcmp(i.name, name) == 0) return i.icon;
    return "\xE2\x9D\x93";
}

bool Contains(const char* hay, const std::string& needle) {
    if (needle.empty()) return true;
    std::string h = hay;
    return h.find(needle) != std::string::npos;
}
}  // namespace

bool EmojiHost::DrawPicker(ImVec2 size, std::string* picked) {
    if (!mod_) return false;
    if (recentLoaded_ != recent) {
        recentLoaded_ = recent;
        recentList_.clear();
        size_t a = 0;
        while (a < recent.size()) {
            size_t b = recent.find('\n', a);
            if (b == std::string::npos) b = recent.size();
            if (b > a) recentList_.push_back(recent.substr(a, b - a));
            a = b + 1;
        }
    }
    bool mobile = ui::M().mobile;
    bool chosen = false;
    ImGui::BeginChild("emojipicker", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    float fs = ImGui::GetFontSize();
    float tabH = mobile ? ui::M().rowH * 0.8f : ImGui::GetFrameHeight() * 1.25f;
    int groups = mod_->group_count();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // ---- tab row: recent, groups, skin tone
    float avail = ImGui::GetContentRegionAvail().x;
    int tabs = groups + 2;
    float tabW = std::min(tabH * 1.3f, (avail - ImGui::GetStyle().ItemSpacing.x * (tabs - 1)) / tabs);
    tabH = std::min(tabH, tabW * 1.15f);  // narrow screens: keep the tabs square-ish
    auto iconButton = [&](const char* id, const std::string& emoji, bool active) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool r = ImGui::InvisibleButton(id, ImVec2(tabW, tabH));
        if (active || ImGui::IsItemHovered())
            dl->AddRectFilled(p, ImVec2(p.x + tabW, p.y + tabH), ImGui::GetColorU32(active ? ImGuiCol_HeaderActive : ImGuiCol_HeaderHovered), 6);
        std::string g = Glyph(emoji);
        float es = std::min(tabW, tabH) * 0.62f;
        ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(es, FLT_MAX, 0, g.c_str());
        dl->AddText(ImGui::GetFont(), es, ImVec2(p.x + (tabW - ts.x) * 0.5f, p.y + (tabH - ts.y) * 0.5f), IM_COL32_WHITE, g.c_str());
        return r;
    };
    static int curGroup = 0;
    if (iconButton("##recent", "\xF0\x9F\x95\x98", curGroup == -1)) scrollToGroup_ = -1, search_.clear();
    if (ImGui::IsItemHovered() && !mobile) ImGui::SetTooltip("%s", T("Recently used"));
    for (int g = 0; g < groups; g++) {
        ImGui::SameLine();
        ImGui::PushID(g);
        if (iconButton("##grp", GroupIcon(mod_->group_name(g)), curGroup == g)) scrollToGroup_ = g, search_.clear();
        if (ImGui::IsItemHovered() && !mobile) ImGui::SetTooltip("%s", T(mod_->group_name(g)));
        ImGui::PopID();
    }
    ImGui::SameLine();
    static const char* hand = "\xE2\x9C\x8B";
    if (iconButton("##tone", WithTone(hand, tone), false)) ImGui::OpenPopup("tones");
    if (ImGui::IsItemHovered() && !mobile) ImGui::SetTooltip("%s", T("Skin tone"));
    if (ImGui::BeginPopup("tones")) {
        for (int t = 0; t <= 5; t++) {
            if (t) ImGui::SameLine();
            ImGui::PushID(t);
            if (iconButton("##t", WithTone(hand, t), t == tone)) {
                tone = t;
                settingsChanged = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }
    // ---- search
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##emojisearch", T("Search emoji"), &search_)) scrollToGroup_ = -2;
    std::string q = search_;
    for (auto& ch : q) ch = (char)tolower((unsigned char)ch);

    // ---- grid: rows of either a group title or emoji
    struct Row {
        int group;  // -1 recent
        bool header;
        int first, count;
    };
    float gridW = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize;
    float cellMin = mobile ? ui::M().rowH * 0.72f : fs * 2.1f;
    int cols = std::max(4, (int)(gridW / cellMin));
    float cell = gridW / cols;
    std::vector<Row> rows;
    std::vector<int> items;  // entry indexes (>= 0) or recent indexes (encoded as -1 - i)
    auto addRows = [&](int group, const std::vector<int>& list) {
        if (list.empty()) return;
        rows.push_back({group, true, 0, 0});
        for (size_t i = 0; i < list.size(); i += (size_t)cols) {
            rows.push_back({group, false, (int)items.size(), (int)std::min<size_t>((size_t)cols, list.size() - i)});
            items.insert(items.end(), list.begin() + (ptrdiff_t)i, list.begin() + (ptrdiff_t)std::min(list.size(), i + (size_t)cols));
        }
    };
    if (q.empty()) {
        std::vector<int> rec;
        for (size_t i = 0; i < recentList_.size(); i++) rec.push_back(-1 - (int)i);
        addRows(-1, rec);
    }
    {
        std::vector<std::vector<int>> per((size_t)groups);
        int n = mod_->entry_count();
        for (int i = 0; i < n; i++) {
            const LseEntry* en = mod_->entry(i);
            if (en->group >= 0 && en->group < groups && Contains(en->name, q)) per[(size_t)en->group].push_back(i);
        }
        for (int g = 0; g < groups; g++) addRows(g, per[(size_t)g]);
    }
    ImGui::BeginChild("emojigrid", ImVec2(0, 0), ImGuiChildFlags_None);
    if (mobile) ui::TouchScroll();
    if (scrollToGroup_ != -3) {
        if (scrollToGroup_ == -2) ImGui::SetScrollY(0);
        else
            for (size_t r = 0; r < rows.size(); r++)
                if (rows[r].header && rows[r].group == scrollToGroup_) {
                    ImGui::SetScrollY(r * cell);
                    break;
                }
        if (scrollToGroup_ == -1 && (rows.empty() || rows[0].group != -1)) ImGui::SetScrollY(0);
        scrollToGroup_ = -3;
    }
    ImGuiIO& io = ImGui::GetIO();
    float es = cell * 0.62f;
    ImGuiListClipper clip;
    clip.Begin((int)rows.size(), cell);
    bool first = true;
    while (clip.Step()) {
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
            const Row& row = rows[(size_t)r];
            if (first) {
                curGroup = row.group;
                first = false;
            }
            ImVec2 p = ImGui::GetCursorScreenPos();
            if (row.header) {
                const char* title = row.group < 0 ? T("Recently used") : T(mod_->group_name(row.group));
                ImGui::Dummy(ImVec2(gridW, cell));
                dl = ImGui::GetWindowDrawList();
                dl->AddText(ImVec2(p.x + fs * 0.3f, p.y + cell - fs * 1.4f), ImGui::GetColorU32(ImGuiCol_TextDisabled), title);
                continue;
            }
            for (int k = 0; k < row.count; k++) {
                int it = items[(size_t)(row.first + k)];
                std::string real;
                const char* name = "";
                if (it < 0) {
                    real = recentList_[(size_t)(-1 - it)];
                } else {
                    const LseEntry* en = mod_->entry(it);
                    real = en->skin_tones ? WithTone(en->emoji, tone) : en->emoji;
                    name = en->name;
                }
                if (k) ImGui::SameLine(0, 0);
                ImGui::PushID(r * 64 + k);
                ImVec2 cp = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton("##e", ImVec2(cell, cell));
                bool hov = ImGui::IsItemHovered();
                bool click = ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(0) && io.MouseDragMaxDistanceSqr[0] < fs * fs * 0.25f && hov;
                ImGui::PopID();
                dl = ImGui::GetWindowDrawList();
                if (hov && !mobile) {
                    dl->AddRectFilled(cp, ImVec2(cp.x + cell, cp.y + cell), ImGui::GetColorU32(ImGuiCol_HeaderHovered), 6);
                    if (*name) ImGui::SetTooltip("%s", name);
                }
                std::string g = Glyph(real);
                ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(es, FLT_MAX, 0, g.c_str());
                dl->AddText(ImGui::GetFont(), es, ImVec2(cp.x + (cell - ts.x) * 0.5f, cp.y + (cell - ts.y) * 0.5f), IM_COL32_WHITE, g.c_str());
                if (click) {
                    *picked = g;
                    chosen = true;
                    // remember it (front of the recent list, at most 32)
                    recentList_.erase(std::remove(recentList_.begin(), recentList_.end(), real), recentList_.end());
                    recentList_.insert(recentList_.begin(), real);
                    if (recentList_.size() > 32) recentList_.resize(32);
                    recent.clear();
                    for (auto& x : recentList_) recent += (recent.empty() ? "" : "\n") + x;
                    recentLoaded_ = recent;
                    settingsChanged = true;
                }
            }
        }
    }
    if (rows.empty()) ImGui::TextDisabled("%s", T("No emoji found"));
    ImGui::EndChild();
    ImGui::EndChild();
    return chosen;
}
