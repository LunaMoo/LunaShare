// LunaShare - internet radio (see radio.h).
#include "radio.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>

#include <SDL.h>

#include "music.h"
#include "net_stream.h"
#include "model.h"
#include "platform.h"

using json = nlohmann::json;

namespace radio {

namespace {
int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
std::string Enc(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else o += '%', o += hex[c >> 4], o += hex[c & 15];
    }
    return o;
}
uint64_t Fnv(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}
std::string PathOf(const std::string& url) {  // without the query
    std::string p = url.substr(0, url.find_first_of("?#"));
    return Lower(p);
}
bool EndsWith(const std::string& s, const char* e) {
    size_t n = strlen(e);
    return s.size() >= n && s.compare(s.size() - n, n, e) == 0;
}
// the system's country (for "popular here"): from the preferred locales, else from the language
std::string SystemCountry() {
    std::string cc, lang;
#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (SDL_Locale* l = SDL_GetPreferredLocales()) {
        for (SDL_Locale* p = l; p->language && cc.empty(); p++) {
            if (lang.empty()) lang = p->language;
            if (p->country && *p->country) cc = p->country;
        }
        SDL_free(l);
    }
#endif
    if (cc.empty() && lang.size() == 2) {
        static const char* map[][2] = {{"pl", "PL"}, {"de", "DE"}, {"fr", "FR"}, {"es", "ES"}, {"it", "IT"}, {"cs", "CZ"},
                                       {"uk", "UA"}, {"ja", "JP"}, {"nl", "NL"}, {"sv", "SE"}, {"pt", "PT"}, {"ru", "RU"}};
        for (auto& m : map)
            if (lang == m[0]) cc = m[1];
    }
    for (auto& c : cc) c = (char)toupper((unsigned char)c);
    return cc;
}
}  // namespace

// ---------------------------------------------------------------- stations

json Station::ToJson() const {
    json j = {{"id", id}, {"name", name}, {"url", url}};
    if (!favicon.empty()) j["favicon"] = favicon;
    if (!tags.empty()) j["tags"] = tags;
    if (!country.empty()) j["country"] = country;
    if (!codec.empty()) j["codec"] = codec;
    if (!homepage.empty()) j["homepage"] = homepage;
    if (bitrate) j["bitrate"] = bitrate;
    if (hls) j["hls"] = true;
    return j;
}

Station Station::FromJson(const json& j) {
    Station s;
    if (!j.is_object()) return s;
    auto str = [&](const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); };
    auto num = [&](const char* k) { return j.contains(k) && j[k].is_number() ? j[k].get<int>() : 0; };
    s.id = str("stationuuid");
    if (s.id.empty()) s.id = str("id");
    s.name = Trim(str("name"));
    s.url = str("url_resolved");  // (Radio Browser: .pls / .m3u already followed)
    if (s.url.empty()) s.url = str("url");
    s.favicon = str("favicon");
    s.tags = str("tags");
    s.country = str("countrycode");
    if (s.country.empty()) s.country = str("country");
    s.codec = str("codec");
    s.homepage = str("homepage");
    s.bitrate = num("bitrate");
    s.votes = num("votes");
    s.hls = (j.contains("hls") && ((j["hls"].is_number() && j["hls"].get<int>() != 0) || (j["hls"].is_boolean() && j["hls"].get<bool>())));
    if (s.id.empty() && !s.url.empty()) s.id = "u:" + s.url;
    return s;
}

// what the player reads through: the stream (with its titles taken out), or HLS playlists and segments
struct Radio::Fetch : music::UrlFetch {
    Radio* r = nullptr;
    uint64_t id = 0;
    bool icy = true;
    std::mutex m;
    std::string lastErr;
    void* Open(const std::string& url, std::atomic<bool>* stop) override {
        auto s = ls::NewNetStream();
        s->cancel = stop;
        std::string err;
        if (!s->Open(url, icy, &err)) {
            std::lock_guard<std::mutex> l(m);
            lastErr = err;
            SDL_Log("radio: %s: %s", url.c_str(), err.c_str());
            return nullptr;
        }
        auto* is = new ls::IcyStream(std::move(s));
        if (icy) {
            Radio* rr = r;
            uint64_t i = id;
            is->onTitle = [rr, i](const std::string& t) { rr->OnTitle(i, t); };
        }
        return is;
    }
    int Read(void* h, uint8_t* buf, int len) override { return ((ls::IcyStream*)h)->Read(buf, len); }
    void Close(void* h) override { delete (ls::IcyStream*)h; }
};

// ---------------------------------------------------------------- set up

Radio& R() {
    static Radio r;
    return r;
}

void Radio::Init(const std::string& configDir) {
    file_ = ls::JoinPath(configDir, "radio.json");
    logoDir_ = ls::JoinPath(ls::JoinPath(configDir, "cache"), "radio");
    ls::MakeDirs(logoDir_);
    Load();
    if (country_.empty()) country_ = SystemCountry();
    th_ = std::thread([this] { Worker(); });
    if (const char* ap = getenv("LUNASHARE_RADIO_PLAY")) {  // (tests: play the first station whose name has this in it)
        std::string want = ap;
        Job([this, want] {
            std::vector<Station> v;
            std::string err;
            if (!Query("/json/stations/topclick/100?hidebroken=true", v, err)) return;
            for (auto& s : v)
                if (s.name.find(want) != std::string::npos) {
                    Play(s);
                    break;
                }
        });
    }
}

void Radio::Shutdown() {
    {
        std::lock_guard<std::mutex> l(m_);
        quit_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

void Radio::Load() {
    json j;
    if (!ls::LoadJsonFile(file_, j) || !j.is_object()) return;
    std::lock_guard<std::mutex> l(m_);
    for (const char* k : {"favourites", "recent"})
        if (j.contains(k) && j[k].is_array())
            for (auto& e : j[k]) {
                Station s = Station::FromJson(e);
                if (!s.url.empty()) (strcmp(k, "favourites") == 0 ? favs_ : recent_).push_back(s);
            }
    if (j.contains("country") && j["country"].is_string()) country_ = j["country"].get<std::string>();
}

void Radio::Save() {
    json j = {{"favourites", json::array()}, {"recent", json::array()}};
    {
        std::lock_guard<std::mutex> l(m_);
        for (auto& s : favs_) j["favourites"].push_back(s.ToJson());
        for (auto& s : recent_) j["recent"].push_back(s.ToJson());
        j["country"] = country_;
    }
    ls::SaveJsonFile(file_, j);
}

void Radio::Worker() {
    for (;;) {
        std::function<void()> fn;
        {
            std::unique_lock<std::mutex> l(m_);
            cv_.wait(l, [&] { return quit_ || !jobs_.empty(); });
            if (quit_) return;
            fn = std::move(jobs_.front());
            jobs_.pop_front();
        }
        fn();
    }
}

void Radio::Job(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> l(m_);
        jobs_.push_back(std::move(fn));
    }
    cv_.notify_one();
}

// ---------------------------------------------------------------- the directory

// one of Radio Browser's servers (they come and go: the list is asked for, with a few known ones behind it)
std::string Radio::Server() {
    if (const char* t = getenv("LUNASHARE_RADIO_API")) return t;  // (tests: a stand-in directory)
    {
        std::lock_guard<std::mutex> l(m_);
        if (!server_.empty()) return server_;
    }
    std::vector<std::string> names;
    std::string body;
    if (ls::HttpGetAll("https://all.api.radio-browser.info/json/servers", body, 1u << 20)) {
        json j = json::parse(body, nullptr, false);
        if (j.is_array())
            for (auto& e : j)
                if (e.is_object() && e.contains("name") && e["name"].is_string()) names.push_back(e["name"].get<std::string>());
    }
    if (names.empty()) names = {"de1.api.radio-browser.info", "fi1.api.radio-browser.info", "de2.api.radio-browser.info"};
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    std::shuffle(names.begin(), names.end(), std::mt19937((unsigned)NowMs()));
    std::lock_guard<std::mutex> l(m_);
    server_ = "https://" + names[0];
    return server_;
}

bool Radio::Query(const std::string& path, std::vector<Station>& out, std::string& err) {
    for (int attempt = 0; attempt < 3; attempt++) {
        std::string body, base = Server();
        if (ls::HttpGetAll(base + path, body, 8u << 20)) {
            json j = json::parse(body, nullptr, false);
            if (!j.is_array()) {
                err = "unexpected answer";
                return false;
            }
            std::vector<std::string> seen;
            for (auto& e : j) {
                Station s = Station::FromJson(e);
                if (s.url.empty() || s.name.empty()) continue;
                std::string key = Lower(s.name);  // (the directory lists many stations twice)
                if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
                seen.push_back(key);
                out.push_back(std::move(s));
            }
            return true;
        }
        std::lock_guard<std::mutex> l(m_);
        if (server_ == base) server_.clear();  // (that server: another one next time)
    }
    err = "can't reach the radio directory";
    return false;
}

void Radio::Popular() {
    std::string cc = CountryCode();
    uint64_t q;
    {
        std::lock_guard<std::mutex> l(m_);
        q = ++query_;
    }
    busy_ = true;
    Job([this, cc, q] {
        std::vector<Station> v;
        std::string err;
        bool ok = Query(cc.empty() ? "/json/stations/topclick/100?hidebroken=true"
                                   : "/json/stations/search?countrycode=" + Enc(cc) + "&order=clickcount&reverse=true&hidebroken=true&limit=100",
                        v, err);
        std::lock_guard<std::mutex> l(m_);
        if (q != query_) return;
        results_ = std::move(v);
        error_ = ok ? std::string() : err;
        busy_ = false;
        version_++;
    });
}

void Radio::Search(const std::string& text) {
    uint64_t q;
    {
        std::lock_guard<std::mutex> l(m_);
        q = ++query_;
    }
    busy_ = true;
    Job([this, text, q] {
        std::vector<Station> v;
        std::string err;
        bool ok = Query("/json/stations/search?name=" + Enc(text) + "&order=clickcount&reverse=true&hidebroken=true&limit=100", v, err);
        std::lock_guard<std::mutex> l(m_);
        if (q != query_) return;
        results_ = std::move(v);
        error_ = ok ? std::string() : err;
        busy_ = false;
        version_++;
    });
}

void Radio::Genre(const std::string& tag) {
    std::string cc = CountryCode();
    uint64_t q;
    {
        std::lock_guard<std::mutex> l(m_);
        q = ++query_;
    }
    busy_ = true;
    Job([this, tag, cc, q] {
        std::vector<Station> v;
        std::string err;
        std::string base = "/json/stations/search?tag=" + Enc(tag) + "&order=clickcount&reverse=true&hidebroken=true&limit=100";
        bool ok = cc.empty() || Query(base + "&countrycode=" + Enc(cc), v, err);
        if (ok && v.size() < 15) {  // few here: the rest from everywhere
            std::vector<Station> more;
            if (Query(base, more, err)) {
                for (auto& s : more)
                    if (std::none_of(v.begin(), v.end(), [&](const Station& x) { return Lower(x.name) == Lower(s.name); })) v.push_back(s);
            } else if (v.empty()) {
                ok = false;
            }
        }
        std::lock_guard<std::mutex> l(m_);
        if (q != query_) return;
        results_ = std::move(v);
        error_ = ok ? std::string() : err;
        busy_ = false;
        version_++;
    });
}

std::vector<Station> Radio::Results() {
    std::lock_guard<std::mutex> l(m_);
    return results_;
}

std::string Radio::Error() {
    std::lock_guard<std::mutex> l(m_);
    return error_;
}

std::string Radio::CountryCode() {
    std::lock_guard<std::mutex> l(m_);
    return country_;
}

void Radio::SetCountry(const std::string& code) {
    {
        std::lock_guard<std::mutex> l(m_);
        country_ = code;
    }
    Save();
}

std::vector<Country> Radio::Countries() {
    {
        std::lock_guard<std::mutex> l(m_);
        if (countriesAsked_) return countries_;
        countriesAsked_ = true;
    }
    Job([this] {
        std::string body;
        std::vector<Country> v;
        if (ls::HttpGetAll(Server() + "/json/countries?hidebroken=true", body, 4u << 20)) {
            json j = json::parse(body, nullptr, false);
            if (j.is_array())
                for (auto& e : j) {
                    if (!e.is_object()) continue;
                    Country c;
                    c.code = e.value("iso_3166_1", "");
                    c.name = e.value("name", "");
                    c.stations = e.contains("stationcount") && e["stationcount"].is_number() ? e["stationcount"].get<int>() : 0;
                    if (c.code.size() == 2 && !c.name.empty() && c.stations >= 3) v.push_back(c);
                }
        }
        std::sort(v.begin(), v.end(), [](const Country& a, const Country& b) { return Lower(a.name) < Lower(b.name); });
        std::lock_guard<std::mutex> l(m_);
        countries_ = std::move(v);
        if (countries_.empty()) countriesAsked_ = false;  // (try again next time)
        version_++;
    });
    return {};
}

// ---------------------------------------------------------------- mine

std::vector<Station> Radio::Favourites() {
    std::lock_guard<std::mutex> l(m_);
    return favs_;
}

std::vector<Station> Radio::Recent() {
    std::lock_guard<std::mutex> l(m_);
    return recent_;
}

bool Radio::IsFavourite(const std::string& id) {
    std::lock_guard<std::mutex> l(m_);
    return std::any_of(favs_.begin(), favs_.end(), [&](const Station& s) { return s.id == id; });
}

void Radio::SetFavourite(const Station& s, bool on) {
    {
        std::lock_guard<std::mutex> l(m_);
        favs_.erase(std::remove_if(favs_.begin(), favs_.end(), [&](const Station& x) { return x.id == s.id; }), favs_.end());
        if (on) favs_.push_back(s);
        version_++;
    }
    Save();
}

Station Radio::AddByUrl(const std::string& url, const std::string& name) {
    Station s;
    s.url = Trim(url);
    s.id = "u:" + s.url;
    s.name = Trim(name).empty() ? s.url : Trim(name);
    s.hls = PathOf(s.url).find(".m3u8") != std::string::npos;
    SetFavourite(s, true);
    return s;
}

// ---------------------------------------------------------------- playing

std::string Radio::LogoFile(const Station& s) {
    if (s.favicon.compare(0, 4, "http") != 0) return {};
    std::string base = ls::JoinPath(logoDir_, std::to_string(Fnv(s.favicon)));
    for (const char* ext : {".png", ".jpg", ".gif", ".webp"})
        if (ls::FileExists(base + ext)) return base + ext;
    FetchLogo(s);
    return {};
}

// a station's logo, once (into the cache: the music player shows covers from files)
void Radio::FetchLogo(const Station& s) {
    {
        std::lock_guard<std::mutex> l(m_);
        if (std::find(logosAsked_.begin(), logosAsked_.end(), s.favicon) != logosAsked_.end()) return;
        logosAsked_.push_back(s.favicon);
    }
    std::string url = s.favicon, base = ls::JoinPath(logoDir_, std::to_string(Fnv(s.favicon)));
    Job([this, url, base] {
        std::string body;
        if (!ls::HttpGetAll(url, body, 1u << 20) || body.size() < 16) return;
        const auto* b = (const unsigned char*)body.data();
        const char* ext = b[0] == 0x89 && b[1] == 'P' ? ".png"
                        : b[0] == 0xFF && b[1] == 0xD8 ? ".jpg"
                        : body.compare(0, 4, "GIF8") == 0 ? ".gif"
                        : body.compare(0, 4, "RIFF") == 0 && body.compare(8, 4, "WEBP") == 0 ? ".webp"
                                                                                           : nullptr;
        if (!ext) return;  // (an .ico or a web page: no logo)
        if (!ls::SaveFileAtomic(base + ext, body)) return;
        version_++;
        if (music::Play().GetNow().extId == playerId_ && playerId_) {  // (the station on: its cover now)
            Station c = Current();
            music::Track t;
            {
                std::lock_guard<std::mutex> l(m_);
                t.title = title_.empty() ? c.name : title_;
                t.artist = title_.empty() ? std::string() : c.name;
            }
            t.cover = base + ext;
            music::Play().SetExternalTrack(playerId_, t);
        }
    });
}

void Radio::Play(const Station& s) {
    {
        std::lock_guard<std::mutex> l(m_);
        cur_ = s;
        title_.clear();
        pausedAt_ = 0;
        recent_.erase(std::remove_if(recent_.begin(), recent_.end(), [&](const Station& x) { return x.id == s.id; }), recent_.end());
        recent_.insert(recent_.begin(), s);
        if (recent_.size() > 30) recent_.resize(30);
        version_++;
    }
    Save();
    std::string server;
    {
        std::lock_guard<std::mutex> l(m_);
        server = server_;
    }
    // what the address is: a stream, a playlist (.pls / .m3u) with the stream in it, or HLS - asked first (the
    // decoder must be told about HLS, and a playlist isn't sound)
    Station st = s;
    Job([this, st, server] {
        if (st.id.compare(0, 2, "u:") != 0 && !server.empty()) {  // (Radio Browser counts what's listened to)
            std::string ignore;
            ls::HttpGetAll(server + "/json/url/" + Enc(st.id), ignore, 64 * 1024);
        }
        std::string url = st.url, fmt = st.hls || PathOf(url).find(".m3u8") != std::string::npos ? "hls" : "";
        if (fmt.empty()) {
            for (int hop = 0; hop < 3; hop++) {
                auto ns = ls::NewNetStream();
                std::string err;
                if (!ns->Open(url, false, &err)) break;  // (the player tries again, and says so)
                std::string ct = Lower(ns->Header("content-type"));
                std::string p = PathOf(ns->finalUrl);
                bool list = ct.find("mpegurl") != std::string::npos || ct.find("scpls") != std::string::npos || ct.find("x-pls") != std::string::npos ||
                            EndsWith(p, ".pls") || EndsWith(p, ".m3u") || EndsWith(p, ".m3u8");
                if (!list) {
                    url = ns->finalUrl;
                    break;
                }
                std::string body;
                uint8_t buf[8192];
                for (int n; (n = ns->Read(buf, sizeof(buf))) > 0 && body.size() < (256u << 10);) body.append((const char*)buf, (size_t)n);
                if (body.find("#EXT-X-") != std::string::npos) {  // HLS
                    url = ns->finalUrl;
                    fmt = "hls";
                    break;
                }
                std::string next;  // the first stream in it ("File1=..." or a plain line)
                size_t pos = 0;
                while (pos < body.size() && next.empty()) {
                    size_t e = body.find('\n', pos);
                    std::string line = Trim(body.substr(pos, e == std::string::npos ? std::string::npos : e - pos));
                    pos = e == std::string::npos ? body.size() : e + 1;
                    size_t eq = line.find('=');
                    if (Lower(line).compare(0, 4, "file") == 0 && eq != std::string::npos) line = Trim(line.substr(eq + 1));
                    if (line.compare(0, 4, "http") == 0) next = ls::ResolveUrl(ns->finalUrl, line);
                }
                if (next.empty()) break;
                url = next;
            }
        }
        Start(st, url, fmt);
    });
}

void Radio::Start(const Station& s, const std::string& url, const std::string& format) {
    {
        std::lock_guard<std::mutex> l(m_);
        if (cur_.id != s.id) return;  // (another station was chosen meanwhile)
    }
    static std::atomic<uint64_t> seq{0};
    uint64_t id = (0x5AD10ull << 40) | (++seq & 0xFFFFFFFFFFull);
    auto f = std::make_shared<Fetch>();
    f->r = this;
    f->id = id;
    f->icy = format != "hls";
    music::Source src;
    src.id = id;
    src.url = url;
    src.format = format;
    src.io = f;
    src.live = true;
    src.track.title = s.name;  // (the song, once the stream says: then the station is the "artist")
    src.track.cover = LogoFile(s);
    {
        std::lock_guard<std::mutex> l(m_);
        fetch_ = f;
    }
    playerId_ = id;
    music::Play().PlayExternal(src, 0, true);
}

void Radio::OnTitle(uint64_t id, const std::string& title) {
    if (id != playerId_) return;
    Station c;
    {
        std::lock_guard<std::mutex> l(m_);
        if (title == title_) return;
        title_ = title;
        c = cur_;
    }
    music::Track t;
    t.title = title.empty() ? c.name : title;
    t.artist = title.empty() ? std::string() : c.name;
    t.cover = LogoFile(c);
    music::Play().SetExternalTrack(id, t);
    version_++;
}

void Radio::Stop() {
    if (Active()) music::Play().EndExternal();
    std::lock_guard<std::mutex> l(m_);
    cur_ = Station();
    playerId_ = 0;
    version_++;
}

bool Radio::Active() {
    uint64_t id = playerId_;
    if (!id) return false;
    music::Now n = music::Play().GetNow();
    return n.external && n.extId == id;
}

bool Radio::Playing(const std::string& id) {
    if (!Active()) return false;
    std::lock_guard<std::mutex> l(m_);
    return cur_.id == id;
}

Station Radio::Current() {
    std::lock_guard<std::mutex> l(m_);
    return cur_;
}

std::string Radio::Title() {
    std::lock_guard<std::mutex> l(m_);
    return title_;
}

bool Radio::Control(int cmd) {
    if (!Active()) return false;
    switch (cmd) {
        case 2:
        case 3: {  // next / previous station: among the favourites (or the ones played lately)
            Station c = Current();
            std::vector<Station> list = Favourites();
            auto has = [&](const std::vector<Station>& v) { return std::any_of(v.begin(), v.end(), [&](const Station& x) { return x.id == c.id; }); };
            if (!has(list)) list = Recent();
            if (list.size() < 2) return true;
            int at = 0;
            for (int i = 0; i < (int)list.size(); i++)
                if (list[(size_t)i].id == c.id) at = i;
            int n = (int)list.size();
            Play(list[(size_t)((at + (cmd == 2 ? 1 : -1) + n) % n)]);
            return true;
        }
        case 4:  // pause: noted (playing again after a while goes back to live)
            pausedAt_ = NowMs();
            return false;
        case 1:
        case 5: {
            music::Now n = music::Play().GetNow();
            if (cmd == 1 && n.playing) {
                pausedAt_ = NowMs();
                return false;
            }
            if (pausedAt_ && NowMs() - pausedAt_ > 5000) {  // (what's buffered is old by now)
                Play(Current());
                return true;
            }
            return false;
        }
        case 6:
            playerId_ = 0;
            return false;
    }
    return false;
}

}  // namespace radio
