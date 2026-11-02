// LunaShare emoji module: the picker catalog plus the platform renderer (emoji_win / emoji_android / emoji_ft).
// SPDX-License-Identifier: GPL-3.0-only
#include <cstring>
#include <string>
#include <vector>

#include "emoji_backend.h"

namespace lse {
namespace {

LseHost g_host{};
bool g_hostSet = false;
std::vector<uint8_t> g_pixels;
std::vector<const char*> g_groups;
std::vector<LseEntry> g_entries;

void BuildCatalog() {
    if (!g_entries.empty()) return;
#define LSE_GROUP(name) g_groups.push_back(name);
#define LSE_EMOJI(e, n, tones) g_entries.push_back({e, n, (int)g_groups.size() - 1, tones});
#include "emoji_catalog.inc"
#undef LSE_GROUP
#undef LSE_EMOJI
}

int Init(const LseHost* h) {
    if (!h || h->api_version != LSE_API_VERSION) return 0;
    g_host = *h;
    g_hostSet = true;
    BuildCatalog();
    std::string why;
    if (!BackendInit(&g_host, &why)) {
        Log("no colour emoji: " + why);
        return 0;
    }
    return 1;
}

const uint8_t* Render(const char* utf8, int size) {
    if (!utf8 || !*utf8 || size < 4 || size > 1024) return nullptr;
    g_pixels.assign((size_t)size * size * 4, 0);
    if (!BackendRender(utf8, size, g_pixels)) return nullptr;
    return g_pixels.data();
}

int GroupCount() { return (int)g_groups.size(); }
const char* GroupName(int g) { return g >= 0 && g < (int)g_groups.size() ? g_groups[(size_t)g] : ""; }
int EntryCount() { return (int)g_entries.size(); }
const LseEntry* Entry(int i) { return i >= 0 && i < (int)g_entries.size() ? &g_entries[(size_t)i] : nullptr; }

const LseModule kModule = {LSE_API_VERSION, "LunaShare Emoji", "1.0", Init, Render, GroupCount, GroupName, EntryCount, Entry};

}  // namespace

void Log(const std::string& msg) {
    if (g_hostSet && g_host.log) g_host.log(g_host.user, ("emoji: " + msg).c_str());
}

}  // namespace lse

extern "C" LSE_EXPORT const LseModule* lunashare_emoji_module(void) { return &lse::kModule; }
