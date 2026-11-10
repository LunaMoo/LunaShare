// LunaShare torrent module: a libtorrent session behind the small C interface of lunashare_torrent.h.
//
// Torrents are kept between runs as resume files (<state>/<id>.resume: what's downloaded, where to, the torrent's
// description once known) and the session's own state (DHT nodes) as session.state. Torrents are not queued: each
// one runs until it is done, then stops or seeds on as the "seed" setting says. Watching while downloading reads
// the file from disk once the pieces under the bytes asked for have arrived (and asks for those first).
//
// SPDX-License-Identifier: GPL-3.0-only
#include "lunashare_torrent.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/ip_filter.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/read_resume_data.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/version.hpp>
#include <libtorrent/write_resume_data.hpp>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using json = nlohmann::json;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

struct Extra {
    bool forceSeed = false;   // started again by hand after it was done: no automatic stop
    bool autoPaused = false;  // stopped because it was done (not by the user)
    std::string savePath;
    // watching: the pieces asked for ahead of the place being read
    int winFirst = -1, winEnd = -1;
    int streamFile = -1;
};

struct State {
    std::mutex m;
    std::unique_ptr<lt::session> ses;
    std::string dir;
    LstHost host{};
    int seed = 0;  // -1 on and on, 0 stop when done, N: until N% uploaded
    std::map<std::string, lt::torrent_handle> h;
    std::map<std::string, Extra> x;
    std::thread pump;
    std::atomic<bool> quit{false};
    std::atomic<int> resumeWaiting{0};
    // the file being read (watching): kept open between reads
    std::mutex fm;
    std::string fpath;
    FILE* f = nullptr;
};
State g;

void Log(const std::string& s) {
    if (g.host.log) g.host.log(g.host.user, ("torrent: " + s).c_str());
}

fs::path U8(const std::string& s) { return fs::u8path(s); }

FILE* OpenU8(const std::string& path, const char* mode) {
#ifdef _WIN32
    std::wstring w = U8(path).wstring(), wm(mode, mode + strlen(mode));
    return _wfopen(w.c_str(), wm.c_str());
#else
    return fopen(path.c_str(), mode);
#endif
}

bool ReadAll(const std::string& path, std::vector<char>& out, size_t max = 64u << 20) {
    FILE* f = OpenU8(path, "rb");
    if (!f) return false;
    out.clear();
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0 && out.size() < max) out.insert(out.end(), buf, buf + n);
    fclose(f);
    return true;
}

bool WriteAll(const std::string& path, const std::vector<char>& data) {
    std::string tmp = path + ".tmp";
    FILE* f = OpenU8(tmp, "wb");
    if (!f) return false;
    bool ok = data.empty() || fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = fclose(f) == 0 && ok;
    std::error_code ec;
    if (ok) fs::rename(U8(tmp), U8(path), ec);
    if (!ok || ec) {
        fs::remove(U8(tmp), ec);
        return false;
    }
    return true;
}

std::string IdOf(const lt::info_hash_t& ih) {
    std::ostringstream o;
    o << ih.get_best();
    return o.str();
}

void SetErr(char* err, int n, const std::string& s) {
    if (!err || n <= 0) return;
    snprintf(err, (size_t)n, "%s", s.c_str());
}

void ApplyPack(lt::settings_pack& p, const json& j) {
    int down = j.value("down", 0), up = j.value("up", 0), port = j.value("port", 0);
    p.set_int(lt::settings_pack::download_rate_limit, down > 0 ? down * 1024 : 0);
    p.set_int(lt::settings_pack::upload_rate_limit, up > 0 ? up * 1024 : 0);
    std::string ps = std::to_string(port > 0 && port < 65536 ? port : 0);
    p.set_str(lt::settings_pack::listen_interfaces, "0.0.0.0:" + ps + ",[::]:" + ps);
    p.set_bool(lt::settings_pack::enable_dht, j.value("dht", true));
    bool upnp = j.value("upnp", true);
    p.set_bool(lt::settings_pack::enable_upnp, upnp);
    p.set_bool(lt::settings_pack::enable_natpmp, upnp);
    p.set_bool(lt::settings_pack::enable_lsd, true);
    std::string agent = j.value("agent", "LunaShare");
    p.set_str(lt::settings_pack::user_agent, agent + " libtorrent/" LIBTORRENT_VERSION);
    g.seed = j.value("seed", 0);
}

json ParseObj(const char* s) {
    json j = s && *s ? json::parse(s, nullptr, false) : json::object();
    return j.is_object() ? j : json::object();
}

void SaveResume(const lt::torrent_handle& h) {
    if (!h.is_valid()) return;
    h.save_resume_data(lt::torrent_handle::save_info_dict);
    g.resumeWaiting++;
}

std::string ResumePath(const std::string& id) { return (U8(g.dir) / (id + ".resume")).u8string(); }

// alerts: resume data to write, things worth a log line. (g.m is NOT held while waiting.)
void HandleAlerts() {
    std::vector<lt::alert*> alerts;
    g.ses->pop_alerts(&alerts);
    for (lt::alert* a : alerts) {
        if (auto* rd = lt::alert_cast<lt::save_resume_data_alert>(a)) {
            g.resumeWaiting--;
            std::string id = IdOf(rd->params.info_hashes);
            if (rd->params.ti) id = IdOf(rd->params.ti->info_hashes());
            bool have;
            {
                std::lock_guard<std::mutex> l(g.m);
                have = g.h.count(id) > 0;
            }
            if (have) WriteAll(ResumePath(id), lt::write_resume_data_buf(rd->params));  // (removed meanwhile: not written back)
        } else if (lt::alert_cast<lt::save_resume_data_failed_alert>(a)) {
            g.resumeWaiting--;
        } else if (auto* md = lt::alert_cast<lt::metadata_received_alert>(a)) {
            SaveResume(md->handle);
        } else if (auto* fin = lt::alert_cast<lt::torrent_finished_alert>(a)) {
            SaveResume(fin->handle);
            Log("finished: " + fin->handle.status(lt::torrent_handle::query_name).name);
        } else if (auto* te = lt::alert_cast<lt::torrent_error_alert>(a)) {
            Log("error: " + te->message());
        } else if (auto* fe = lt::alert_cast<lt::file_error_alert>(a)) {
            Log("file error: " + fe->message());
        } else if (auto* lf = lt::alert_cast<lt::listen_failed_alert>(a)) {
            Log(lf->message());
        }
    }
}

// done torrents stop (or seed up to the share set); every couple of seconds
void SeedRules() {
    std::vector<std::pair<std::string, lt::torrent_handle>> hs;
    int seed;
    {
        std::lock_guard<std::mutex> l(g.m);
        hs.assign(g.h.begin(), g.h.end());
        seed = g.seed;
    }
    for (auto& [id, h] : hs) {
        if (!h.is_valid()) continue;
        lt::torrent_status st = h.status({});
        bool paused = bool(st.flags & lt::torrent_flags::paused);
        if (paused || !st.is_finished || !st.has_metadata || st.total_wanted <= 0) continue;
        if (st.state != lt::torrent_status::finished && st.state != lt::torrent_status::seeding) continue;
        bool stop = seed == 0 || (seed > 0 && st.all_time_upload * 100 >= st.total_wanted * (std::int64_t)seed);
        if (!stop) continue;
        {
            std::lock_guard<std::mutex> l(g.m);
            auto it = g.x.find(id);
            if (it == g.x.end() || it->second.forceSeed) continue;
            it->second.autoPaused = true;
        }
        h.pause();
        SaveResume(h);
    }
}

void Pump() {
    auto lastRules = Clock::now(), lastSave = Clock::now();
    while (!g.quit) {
        g.ses->wait_for_alert(std::chrono::milliseconds(300));
        HandleAlerts();
        auto now = Clock::now();
        if (now - lastRules > std::chrono::seconds(2)) {
            lastRules = now;
            SeedRules();
        }
        if (now - lastSave > std::chrono::seconds(90)) {  // (what's downloaded so far survives a crash)
            lastSave = now;
            std::vector<lt::torrent_handle> hs;
            {
                std::lock_guard<std::mutex> l(g.m);
                for (auto& [id, h] : g.h) hs.push_back(h);
            }
            for (auto& h : hs)
                if (h.is_valid() && h.need_save_resume_data()) SaveResume(h);
        }
    }
}

bool Find(const char* id, lt::torrent_handle& h) {
    if (!id) return false;
    std::lock_guard<std::mutex> l(g.m);
    auto it = g.h.find(id);
    if (it == g.h.end() || !it->second.is_valid()) return false;
    h = it->second;
    return true;
}

#ifndef _WIN32
// OpenSSL is linked in: its built-in idea of where the certificates are may not fit this system
void FindCertificates() {
    if (getenv("SSL_CERT_FILE")) return;
    for (const char* p : {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/ca-bundle.pem", "/etc/ssl/cert.pem",
                          "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem"}) {
        std::error_code ec;
        if (fs::exists(p, ec)) {
            setenv("SSL_CERT_FILE", p, 0);
            return;
        }
    }
}
#endif

int Init(const LstHost* host, const char* stateDir, const char* settingsJson) {
    if (!host || host->api_version != LST_API_VERSION || !stateDir) return 0;
    if (g.ses) return 1;
    g.host = *host;
    g.dir = stateDir;
    std::error_code fec;
    fs::create_directories(U8(g.dir), fec);
#ifndef _WIN32
    FindCertificates();
#endif
    try {
        lt::session_params sp;
        std::vector<char> buf;
        if (ReadAll((U8(g.dir) / "session.state").u8string(), buf) && !buf.empty()) {
            lt::error_code ec;
            lt::bdecode_node n = lt::bdecode(buf, ec);
            if (!ec) sp = lt::read_session_params(n, lt::session::save_dht_state);
        }
        lt::settings_pack& p = sp.settings;
        p.set_int(lt::settings_pack::alert_mask, lt::alert_category::status | lt::alert_category::error | lt::alert_category::storage);
        p.set_str(lt::settings_pack::dht_bootstrap_nodes,
                  "dht.libtorrent.org:25401,router.bittorrent.com:6881,router.utorrent.com:6881,dht.transmissionbt.com:6881");
        p.set_int(lt::settings_pack::active_downloads, -1);  // (no queue: every torrent runs)
        p.set_int(lt::settings_pack::active_seeds, -1);
        p.set_int(lt::settings_pack::active_limit, -1);
        ApplyPack(p, ParseObj(settingsJson));
        json sj = ParseObj(settingsJson);
        g.ses = std::make_unique<lt::session>(std::move(sp));
        // (libtorrent leaves peers on the local network out of the speed limits; "limitLocal" counts them in - tests)
        if (sj.value("limitLocal", false)) g.ses->set_peer_class_filter(lt::ip_filter());
    } catch (const std::exception& e) {
        Log(std::string("can't start: ") + e.what());
        g.ses.reset();
        return 0;
    }
    // the torrents from last time
    int n = 0;
    for (auto& e : fs::directory_iterator(U8(g.dir), fec)) {
        if (e.path().extension() != ".resume") continue;
        std::vector<char> buf;
        if (!ReadAll(e.path().u8string(), buf) || buf.empty()) continue;
        lt::error_code ec;
        lt::add_torrent_params p = lt::read_resume_data(buf, ec);
        if (ec) continue;
        p.flags &= ~lt::torrent_flags::auto_managed;
        lt::torrent_handle h = g.ses->add_torrent(std::move(p), ec);
        if (ec || !h.is_valid()) continue;
        std::string id = IdOf(h.info_hashes());
        std::lock_guard<std::mutex> l(g.m);
        g.h[id] = h;
        g.x[id];
        n++;
    }
    g.quit = false;
    g.pump = std::thread(Pump);
    Log("started (libtorrent " LIBTORRENT_VERSION "), " + std::to_string(n) + " torrent(s)");
    return 1;
}

void Shutdown() {
    if (!g.ses) return;
    g.quit = true;
    if (g.pump.joinable()) g.pump.join();
    {
        std::lock_guard<std::mutex> l(g.fm);
        if (g.f) fclose(g.f);
        g.f = nullptr;
        g.fpath.clear();
    }
    try {
        g.ses->pause();
        HandleAlerts();
        g.resumeWaiting = 0;
        std::vector<lt::torrent_handle> hs;
        {
            std::lock_guard<std::mutex> l(g.m);
            for (auto& [id, h] : g.h) hs.push_back(h);
        }
        for (auto& h : hs) SaveResume(h);
        auto until = Clock::now() + std::chrono::seconds(6);
        while (g.resumeWaiting > 0 && Clock::now() < until) {
            g.ses->wait_for_alert(std::chrono::milliseconds(200));
            HandleAlerts();
        }
        WriteAll((U8(g.dir) / "session.state").u8string(), lt::write_session_params_buf(g.ses->session_state(lt::session::save_dht_state)));
    } catch (const std::exception&) {
    }
    g.ses.reset();
    std::lock_guard<std::mutex> l(g.m);
    g.h.clear();
    g.x.clear();
}

void ApplySettings(const char* settingsJson) {
    if (!g.ses) return;
    lt::settings_pack p;
    ApplyPack(p, ParseObj(settingsJson));
    g.ses->apply_settings(p);
}

int AddParams(lt::add_torrent_params p, const char* optionsJson, char* idOut, char* err, int errLen) {
    json o = ParseObj(optionsJson);
    std::string dir = o.value("dir", "");
    if (dir.empty()) {
        SetErr(err, errLen, "no folder to save into");
        return 0;
    }
    std::string id = p.ti ? IdOf(p.ti->info_hashes()) : IdOf(p.info_hashes);
    if (idOut) snprintf(idOut, 41, "%s", id.c_str());
    {
        std::lock_guard<std::mutex> l(g.m);
        if (g.h.count(id)) return 1;  // (there already)
    }
    std::error_code fec;
    fs::create_directories(U8(dir), fec);
    p.save_path = dir;
    p.flags &= ~lt::torrent_flags::auto_managed;
    if (o.value("paused", false)) p.flags |= lt::torrent_flags::paused;
    else p.flags &= ~lt::torrent_flags::paused;
    lt::error_code ec;
    lt::torrent_handle h = g.ses->add_torrent(std::move(p), ec);
    if (ec || !h.is_valid()) {
        SetErr(err, errLen, ec ? ec.message() : "can't add it");
        return 0;
    }
    {
        std::lock_guard<std::mutex> l(g.m);
        g.h[id] = h;
        g.x[id].savePath = dir;
    }
    SaveResume(h);
    return 1;
}

int AddData(const uint8_t* data, int size, const char* optionsJson, char* idOut, char* err, int errLen) {
    if (!g.ses || !data || size <= 0) return 0;
    try {
        lt::error_code ec;
        auto ti = std::make_shared<lt::torrent_info>(lt::span<char const>((const char*)data, size), ec, lt::from_span);
        if (ec) {
            SetErr(err, errLen, "not a torrent file (" + ec.message() + ")");
            return 0;
        }
        lt::add_torrent_params p;
        p.ti = ti;
        return AddParams(std::move(p), optionsJson, idOut, err, errLen);
    } catch (const std::exception& e) {
        SetErr(err, errLen, e.what());
        return 0;
    }
}

int Add(const char* source, const char* optionsJson, char* idOut, char* err, int errLen) {
    if (!g.ses || !source) return 0;
    std::string s = source;
    try {
        if (s.compare(0, 7, "magnet:") == 0) {
            lt::error_code ec;
            lt::add_torrent_params p = lt::parse_magnet_uri(s, ec);
            if (ec) {
                SetErr(err, errLen, "not a magnet link (" + ec.message() + ")");
                return 0;
            }
            return AddParams(std::move(p), optionsJson, idOut, err, errLen);
        }
        std::vector<char> buf;
        if (!ReadAll(s, buf) || buf.empty()) {
            SetErr(err, errLen, "can't read the file");
            return 0;
        }
        return AddData((const uint8_t*)buf.data(), (int)buf.size(), optionsJson, idOut, err, errLen);
    } catch (const std::exception& e) {
        SetErr(err, errLen, e.what());
        return 0;
    }
}

std::string Dump(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

const char* List() {
    static thread_local std::string out;
    json arr = json::array();
    if (g.ses) {
        std::vector<std::pair<std::string, lt::torrent_handle>> hs;
        {
            std::lock_guard<std::mutex> l(g.m);
            hs.assign(g.h.begin(), g.h.end());
        }
        for (auto& [id, h] : hs) {
            if (!h.is_valid()) continue;
            try {
                lt::torrent_status st = h.status(lt::torrent_handle::query_name | lt::torrent_handle::query_save_path | lt::torrent_handle::query_torrent_file);
                bool paused = bool(st.flags & lt::torrent_flags::paused);
                std::string state;
                if (st.errc) state = "error";
                else if (paused) state = st.has_metadata && st.is_finished && st.total_wanted > 0 ? "done" : "paused";
                else if (st.state == lt::torrent_status::checking_files || st.state == lt::torrent_status::checking_resume_data) state = "checking";
                else if (st.state == lt::torrent_status::downloading_metadata || !st.has_metadata) state = "meta";
                else if (st.state == lt::torrent_status::finished || st.state == lt::torrent_status::seeding) state = "seed";
                else state = "down";
                auto ti = st.torrent_file.lock();
                int files = ti ? ti->num_files() : 0;
                bool single = false;
                std::string path = st.save_path;
                if (ti) {
                    std::string first = files > 0 ? ti->files().file_path(lt::file_index_t(0)) : "";
                    single = files == 1 && first.find_first_of("/\\") == std::string::npos;
                    path = (U8(st.save_path) / U8(single ? first : ti->name())).u8string();
                }
                {
                    std::lock_guard<std::mutex> l(g.m);
                    auto it = g.x.find(id);
                    if (it != g.x.end()) it->second.savePath = st.save_path;
                }
                arr.push_back({{"id", id},
                               {"name", st.name.empty() ? id : st.name},
                               {"dir", st.save_path},
                               {"path", path},
                               {"state", state},
                               {"error", st.errc ? st.errc.message() : ""},
                               {"size", st.total_wanted},
                               {"done", st.total_wanted_done},
                               {"downRate", paused ? 0 : st.download_payload_rate},
                               {"upRate", paused ? 0 : st.upload_payload_rate},
                               {"uploaded", st.all_time_upload},
                               {"peers", st.num_peers},
                               {"seeds", st.num_seeds},
                               {"files", files},
                               {"added", (std::int64_t)st.added_time},
                               {"paused", paused},
                               {"single", single}});
            } catch (const std::exception&) {
            }
        }
    }
    out = Dump(arr);
    return out.c_str();
}

const char* Files(const char* id) {
    static thread_local std::string out;
    json arr = json::array();
    lt::torrent_handle h;
    if (g.ses && Find(id, h)) {
        try {
            auto ti = h.torrent_file();
            if (ti) {
                std::vector<std::int64_t> prog = h.file_progress(lt::torrent_handle::piece_granularity);
                std::vector<lt::download_priority_t> prio = h.get_file_priorities();
                const lt::file_storage& st = ti->files();
                for (int i = 0; i < st.num_files(); i++) {
                    lt::file_index_t fi(i);
                    if (st.pad_file_at(fi)) continue;
                    arr.push_back({{"i", i},
                                   {"path", st.file_path(fi)},
                                   {"size", st.file_size(fi)},
                                   {"done", i < (int)prog.size() ? prog[(size_t)i] : 0},
                                   {"want", i >= (int)prio.size() || prio[(size_t)i] != lt::dont_download}});
                }
            }
        } catch (const std::exception&) {
        }
    }
    out = Dump(arr);
    return out.c_str();
}

void SetWanted(const char* id, const char* wantedJson) {
    lt::torrent_handle h;
    if (!g.ses || !Find(id, h)) return;
    json w = wantedJson ? json::parse(wantedJson, nullptr, false) : json();
    if (!w.is_array()) return;
    try {
        auto ti = h.torrent_file();
        if (!ti) return;
        std::vector<lt::download_priority_t> prio = h.get_file_priorities();
        prio.resize((size_t)ti->num_files(), lt::default_priority);
        for (size_t i = 0; i < prio.size() && i < w.size(); i++) {
            bool want = w[i].is_boolean() ? w[i].get<bool>() : true;
            if (!want) prio[i] = lt::dont_download;
            else if (prio[i] == lt::dont_download) prio[i] = lt::default_priority;
        }
        h.prioritize_files(prio);
        bool resume = false;
        {
            std::lock_guard<std::mutex> l(g.m);
            auto it = g.x.find(id);
            if (it != g.x.end() && it->second.autoPaused) {  // (it was done: more to fetch now)
                it->second.autoPaused = false;
                resume = true;
            }
        }
        if (resume) h.resume();
        SaveResume(h);
    } catch (const std::exception&) {
    }
}

void Pause(const char* id, int paused) {
    lt::torrent_handle h;
    if (!g.ses || !Find(id, h)) return;
    try {
        if (paused) {
            h.pause();
        } else {
            lt::torrent_status st = h.status({});
            {
                std::lock_guard<std::mutex> l(g.m);
                Extra& x = g.x[id];
                x.autoPaused = false;
                if (st.has_metadata && st.is_finished) x.forceSeed = true;  // (started by hand: it seeds until stopped by hand)
            }
            h.clear_error();
            h.resume();
        }
        SaveResume(h);
    } catch (const std::exception&) {
    }
}

void Remove(const char* id, int deleteFiles) {
    lt::torrent_handle h;
    if (!g.ses || !Find(id, h)) return;
    {
        std::lock_guard<std::mutex> l(g.m);
        g.h.erase(id);
        g.x.erase(id);
    }
    {
        std::lock_guard<std::mutex> l(g.fm);  // (a file of it may be open for watching)
        if (g.f) fclose(g.f);
        g.f = nullptr;
        g.fpath.clear();
    }
    try {
        g.ses->remove_torrent(h, deleteFiles ? lt::session::delete_files : lt::remove_flags_t{});
    } catch (const std::exception&) {
    }
    std::error_code ec;
    fs::remove(U8(ResumePath(id)), ec);
}

void Stream(const char* id, int file, int on) {
    lt::torrent_handle h;
    if (!g.ses || !Find(id, h)) return;
    try {
        if (on) h.set_flags(lt::torrent_flags::sequential_download);
        else h.unset_flags(lt::torrent_flags::sequential_download);
        h.clear_piece_deadlines();
        auto ti = h.torrent_file();
        {
            std::lock_guard<std::mutex> l(g.m);
            Extra& x = g.x[id];
            x.streamFile = on ? file : -1;
            x.winFirst = x.winEnd = -1;
        }
        if (!ti || file < 0 || file >= ti->num_files()) return;
        lt::file_index_t fi(file);
        if (on) {
            if (h.file_priority(fi) == lt::dont_download) h.file_priority(fi, lt::default_priority);
            // the beginning and the end first: players read both before the picture shows
            const lt::file_storage& st = ti->files();
            std::int64_t off = st.file_offset(fi), size = st.file_size(fi);
            int plen = st.piece_length();
            int first = (int)(off / plen), last = (int)((off + std::max<std::int64_t>(size, 1) - 1) / plen);
            h.set_piece_deadline(lt::piece_index_t(first), 0);
            h.set_piece_deadline(lt::piece_index_t(last), 500);
        }
    } catch (const std::exception&) {
    }
}

int Read(const char* id, int file, int64_t offset, uint8_t* buf, int len, int timeoutMs, int* timedOut) {
    if (timedOut) *timedOut = 0;
    if (!g.ses || !buf || len <= 0 || offset < 0) return -1;
    auto t0 = Clock::now();
    auto late = [&] { return Clock::now() - t0 > std::chrono::milliseconds(std::max(timeoutMs, 0)); };
    try {
        for (;;) {
            lt::torrent_handle h;
            if (!Find(id, h)) return -1;
            auto ti = h.torrent_file();
            if (!ti) {  // (a magnet link: the description hasn't come yet)
                if (late()) {
                    if (timedOut) *timedOut = 1;
                    return 0;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            const lt::file_storage& st = ti->files();
            if (file < 0 || file >= st.num_files()) return -1;
            lt::file_index_t fi(file);
            std::int64_t fsize = st.file_size(fi);
            if (offset >= fsize) return 0;
            std::int64_t want = std::min<std::int64_t>(len, fsize - offset);
            std::int64_t toff = st.file_offset(fi) + offset;
            int plen = st.piece_length();
            int p0 = (int)(toff / plen), p1 = (int)((toff + want - 1) / plen);
            int fileLast = (int)((st.file_offset(fi) + fsize - 1) / plen);
            // what's ahead is asked for first (about 12 MB, at least 4 pieces); a jump elsewhere drops the old asks
            int ahead = std::max(4, (12 << 20) / std::max(plen, 1));
            std::string savePath;
            bool ask = false;
            {
                std::lock_guard<std::mutex> l(g.m);
                Extra& x = g.x[id];
                savePath = x.savePath;
                bool inside = x.winFirst >= 0 && p0 >= x.winFirst && p0 < x.winEnd;
                if (!inside || p0 + ahead / 2 >= x.winEnd) {
                    ask = true;
                    if (!inside) x.winFirst = -1;
                }
            }
            if (ask) {
                bool jumped;
                {
                    std::lock_guard<std::mutex> l(g.m);
                    Extra& x = g.x[id];
                    jumped = x.winFirst < 0;
                    x.winFirst = p0;
                    x.winEnd = std::min(fileLast + 1, p0 + ahead);
                }
                if (jumped) h.clear_piece_deadlines();
                int end = std::min(fileLast + 1, p0 + ahead);
                for (int p = p0, k = 0; p < end; p++, k++)
                    if (!h.have_piece(lt::piece_index_t(p))) h.set_piece_deadline(lt::piece_index_t(p), 150 * k);
            }
            // the bytes that are whole pieces from here on
            int have = p0;
            while (have <= p1 && h.have_piece(lt::piece_index_t(have))) have++;
            if (have > p0) {
                std::int64_t end = std::min<std::int64_t>(toff + want, (std::int64_t)have * plen);
                int n = (int)(end - toff);
                if (savePath.empty()) savePath = h.status(lt::torrent_handle::query_save_path).save_path;
                std::string path = (U8(savePath) / U8(st.file_path(fi))).u8string();
                std::lock_guard<std::mutex> l(g.fm);
                if (g.fpath != path || !g.f) {
                    if (g.f) fclose(g.f);
                    g.f = OpenU8(path, "rb");
                    g.fpath = g.f ? path : "";
                }
                if (g.f) {
#ifdef _WIN32
                    _fseeki64(g.f, offset, SEEK_SET);
#else
                    fseeko(g.f, (off_t)offset, SEEK_SET);
#endif
                    size_t r = fread(buf, 1, (size_t)n, g.f);
                    if (r > 0) return (int)r;
                    fclose(g.f);  // (not on disk yet: once more in a moment)
                    g.f = nullptr;
                    g.fpath.clear();
                }
            }
            if (late()) {
                if (timedOut) *timedOut = 1;
                return 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        }
    } catch (const std::exception&) {
        return -1;
    }
}

void AddPeer(const char* id, const char* ip, int port) {
    lt::torrent_handle h;
    if (!g.ses || !ip || !Find(id, h)) return;
    try {
        lt::error_code ec;
        auto addr = lt::make_address(ip, ec);
        if (!ec) h.connect_peer(lt::tcp::endpoint(addr, (unsigned short)port));
    } catch (const std::exception&) {
    }
}

const LstModule kModule = {LST_API_VERSION, "libtorrent", LIBTORRENT_VERSION, Init,   Shutdown, ApplySettings, Add,
                           AddData,         List,         Files,              SetWanted, Pause,  Remove,        Stream,
                           Read,            AddPeer};

}  // namespace

extern "C" LST_EXPORT const LstModule* lunashare_torrent_module(void) { return &kModule; }
