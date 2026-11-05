// SPDX-License-Identifier: GPL-3.0-only
#include "i18n.h"

#include <SDL.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

struct Pair {
    const char* en;
    const char* tr;
};

const Pair kPolish[] = {
#include "lang/pl.inc"
};

struct Table {
    const char* code;
    const Pair* pairs;
    size_t count;
};
const Table kTables[] = {{"pl", kPolish, sizeof(kPolish) / sizeof(kPolish[0])}};

std::unordered_map<std::string_view, const char*> g_map;  // English -> current language
std::unordered_map<const char*, const char*> g_cache;     // literal address -> result
std::deque<std::string> g_store;                          // results that had to be built ("text##id")
std::string g_code = "en";
int g_gen = 0;

std::string SystemLanguage() {
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (SDL_Locale* l = SDL_GetPreferredLocales()) {
        std::string code;
        for (SDL_Locale* p = l; p->language; p++) {
            for (auto& lang : Languages())
                if (strcmp(p->language, lang.code) == 0) {
                    code = lang.code;
                    break;
                }
            if (!code.empty()) break;
        }
        SDL_free(l);
        if (!code.empty()) return code;
    }
#endif
    return "en";
}

const char* Lookup(std::string_view en) {
    auto it = g_map.find(en);
    return it == g_map.end() ? nullptr : it->second;
}

}  // namespace

const std::vector<Language>& Languages() {
    static const std::vector<Language> v = {{"en", "English"}, {"pl", "Polski"}};
    return v;
}

void SetLanguage(const std::string& code) {
    std::string c = code.empty() ? SystemLanguage() : code;
    g_map.clear();
    g_cache.clear();
    g_store.clear();
    g_code = "en";
    for (auto& t : kTables)
        if (c == t.code) {
            g_code = t.code;
            for (size_t i = 0; i < t.count; i++) g_map[t.pairs[i].en] = t.pairs[i].tr;
        }
    g_gen++;
}

std::string CurrentLanguage() { return g_code; }
int LanguageGeneration() { return g_gen; }

const char* T(const char* en) {
    if (!en || g_map.empty()) return en;
    auto c = g_cache.find(en);
    if (c != g_cache.end()) return c->second;
    const char* out = en;
    const char* hash = strstr(en, "##");
    if (!hash) {
        if (const char* t = Lookup(en)) out = t;
    } else if (const char* t = Lookup(std::string_view(en, (size_t)(hash - en)))) {
        g_store.push_back(std::string(t) + hash);  // keep the ID part
        out = g_store.back().c_str();
    }
    g_cache[en] = out;
    return out;
}

std::string TS(const std::string& en) {
    if (g_map.empty()) return en;
    size_t hash = en.find("##");
    if (const char* t = Lookup(std::string_view(en).substr(0, hash))) return std::string(t) + (hash == std::string::npos ? "" : en.substr(hash));
    return en;
}

const char* TItems(const char* items) {
    if (!items || g_map.empty()) return items;
    auto c = g_cache.find(items);
    if (c != g_cache.end()) return c->second;
    std::string out;
    for (const char* p = items; *p; p += strlen(p) + 1) {
        const char* t = Lookup(p);
        out += t ? t : p;
        out += '\0';
    }
    out += '\0';
    g_store.push_back(out);
    const char* r = g_store.back().data();
    g_cache[items] = r;
    return r;
}

std::string TF(const char* fmt, ...) {
    const char* f = T(fmt);
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    if (n < (int)sizeof(buf)) return std::string(buf, n > 0 ? (size_t)n : 0);
    std::string big((size_t)n + 1, '\0');
    va_start(ap, fmt);
    vsnprintf(&big[0], big.size(), f, ap);
    va_end(ap);
    big.resize((size_t)n);
    return big;
}

// Strings that reach T()/TS()/TDynamic() from tables or from the core (picked up by tools/i18n_keys.py):
// i18n-keys: "Auto", "Daylight", "Cloudy", "Shade", "Tungsten (indoor lamps)", "Fluorescent",
// i18n-keys: "Smileys & Emotion", "People & Body", "Animals & Nature", "Food & Drink", "Travel & Places", "Activities", "Objects", "Symbols", "Flags",
// i18n-keys: "LAN", "Internet", "Bluetooth", "broadcast", "mDNS", "internet", "broadcast + mDNS", "mDNS + internet", "broadcast + internet", "broadcast + mDNS + internet",
// i18n-keys: "Call not answered - busy", "Call declined", "Call failed - the other device has no voice module", "No answer", "Call cancelled",
// i18n-keys: "Declined call", "Missed call", "Missed call - calls are not installed on this device (voice module missing)", "Missed call (you were in another call)",
// i18n-keys: "server not responding", "server not responding (check address, port and firewall)", "connecting to server", "invalid server address", "off", "starting",
// i18n-keys: "Hashing", "Waiting for accept", "Incoming", "Queued", "Transferring", "Paused", "Waiting for device", "Verifying", "Done", "Failed", "Declined", "Cancelled",
// i18n-keys: "Desktop", "Documents", "Downloads", "Pictures", "Videos", "Music"
// i18n-keys: "Speaker", "could not open the picture", "could not encode the picture", "not a JPEG picture or too large", "could not save the picture",
// i18n-keys: "This device was removed from your account and now has an account of its own.", "the words don't match (a typo or a word missing?)",
// i18n-keys: "use a password of at least 6 characters", "could not write the file", "not a LunaShare account backup", "wrong password (or the file is damaged)",
// i18n-keys: "the backup is damaged", "the account key is not valid", "the account's device list is not valid", "This device was linked to another account.",
// i18n-keys: "Call answered on another device", "this device has no account", "invalid signature", "server full"
// i18n-keys: "You created the group.", "You left the group.", "Group", "Declined group call", "Missed group call",
// i18n-keys: "Missed group call (you were in another call)", "Missed group call - calls are not installed on this device (voice module missing)",
// i18n-keys: "Incoming group call", "this server doesn't relay calls", "call full",
// i18n-keys: "Watching together ended (connection lost).", "Watch together: could not create a temporary file", "Sound %d", "Subtitles %d"
// Sentences the core writes (in English, also into the chat history). "%s" parts are copied over.
static const char* const kPatterns[] = {
    "End-to-end encrypted. Safety number: %s - compare it on both devices to make sure nobody is in between.",
    "Identity key changed (new fingerprint %s). Connection refused until you accept the new key in Devices.",
    "Recognised hardware of \"%s\" (new app identity) - nickname copied.",
    "New identity key accepted. Safety number: %s",
    "New device: %s (%s)",
    "Linking was declined on %s.",
    "%s is now linked to your account.",
    "This device is now linked to the account of %s.",
    "unknown word: %s",
    "the recovery phrase has 24 words (got %s)",
    "Outgoing call - %s (connection lost)",
    "Incoming call - %s (connection lost)",
    "Outgoing call - %s",
    "Incoming call - %s",
    "connected - %s other device(s) in the room",
    "cannot resolve %s",
    "UDP port %s is busy, using a random one",
    "server: %s",
    "You added %s.",
    "You removed %s.",
    "You named the group \"%s\".",
    "%s added you to the group.",
    "%s removed you from the group.",
    "%s left the group.",
    "%s named the group \"%s\".",
    "%s added %s.",
    "%s removed %s.",
    "Group call - %s",
    "You started watching \"%s\" together.",
    "%s invites you to watch \"%s\" together.",
    "%s stopped the movie.",
    "Group call through the server: %s",
};

std::string TDynamic(const std::string& en) {
    if (g_map.empty() || en.empty()) return en;
    if (const char* t = Lookup(en)) return t;
    for (const char* pat : kPatterns) {
        // split the pattern at %s and match the parts in order
        std::vector<std::string> parts;
        std::string p = pat;
        size_t a = 0, b;
        while ((b = p.find("%s", a)) != std::string::npos) {
            parts.push_back(p.substr(a, b - a));
            a = b + 2;
        }
        parts.push_back(p.substr(a));
        if (en.compare(0, parts[0].size(), parts[0]) != 0) continue;
        size_t pos = parts[0].size();
        std::vector<std::string> vars;
        bool ok = true;
        for (size_t i = 1; i < parts.size() && ok; i++) {
            size_t at = i + 1 == parts.size() ? (en.size() >= parts[i].size() ? en.size() - parts[i].size() : std::string::npos)
                                               : en.find(parts[i], pos);
            if (at == std::string::npos || at < pos || en.compare(at, parts[i].size(), parts[i]) != 0) ok = false;
            else {
                vars.push_back(en.substr(pos, at - pos));
                pos = at + parts[i].size();
            }
        }
        if (!ok || pos != en.size()) continue;
        const char* tr = Lookup(pat);
        if (!tr) return en;
        std::string out, f = tr;
        size_t vi = 0;
        for (size_t i = 0; i < f.size(); i++) {
            if (f[i] == '%' && i + 1 < f.size() && f[i + 1] == 's') {
                if (vi < vars.size()) out += TDynamic(vars[vi]);
                vi++;
                i++;
            } else {
                out += f[i];
            }
        }
        return out;
    }
    return en;
}
