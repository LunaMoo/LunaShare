// LunaShare - internet radio: stations from the Radio Browser directory (radio-browser.info: free, community-run, no
// account), favourites and the ones played lately, and a station playing through the music player (its title from
// the stream, its logo as the cover). Plain streams (MP3 / AAC / Ogg over http(s), Icecast / Shoutcast) and HLS.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace radio {

struct Station {
    std::string id;       // Radio Browser's stationuuid ("u:" + URL for one added by hand)
    std::string name, url, favicon, tags, country, codec, homepage;
    int bitrate = 0, votes = 0;
    bool hls = false;
    nlohmann::json ToJson() const;
    static Station FromJson(const nlohmann::json& j);  // also Radio Browser's own fields
};

struct Country {
    std::string code, name;
    int stations = 0;
};

class Radio {
public:
    void Init(const std::string& configDir);
    void Shutdown();

    // ---- the directory (asked in the background; the UI looks at Results() when Version() changes)
    void Popular();                                // the most listened-to in my country
    void Search(const std::string& text);          // by name
    void Genre(const std::string& tag);            // by tag (in my country first, then anywhere)
    bool Busy() const { return busy_; }
    std::vector<Station> Results();
    std::string Error();
    uint64_t Version() const { return version_; }
    std::string CountryCode();                     // the one popular stations come from ("" = everywhere)
    void SetCountry(const std::string& code);
    std::vector<Country> Countries();              // (fetched once, when first asked)

    // ---- mine
    std::vector<Station> Favourites();
    std::vector<Station> Recent();
    bool IsFavourite(const std::string& id);
    void SetFavourite(const Station& s, bool on);
    // a station by its address (an http(s) stream, an .m3u / .pls playlist or an HLS .m3u8): kept as a favourite
    Station AddByUrl(const std::string& url, const std::string& name);

    // ---- playing
    void Play(const Station& s);
    void Stop();
    bool Playing(const std::string& id);           // this station is the one on
    bool Active();                                 // a station is on (or connecting)
    Station Current();
    std::string Title();                           // the song now on (from the stream), "" = not known
    std::string LogoFile(const Station& s);        // its logo once fetched (the cover), "" before / without
    // the player's buttons while a station is on: next / previous station (favourites, else the list it came from),
    // play after a pause goes back to live. true = handled
    bool Control(int cmd);
    uint64_t PlayerId() const { return playerId_; }

private:
    void Worker();
    void Job(std::function<void()> fn);
    bool Query(const std::string& path, std::vector<Station>& out, std::string& err);
    std::string Server();
    void Start(const Station& s, const std::string& url, const std::string& format);
    void FetchLogo(const Station& s);
    void Load();
    void Save();
    void OnTitle(uint64_t id, const std::string& title);

    std::string file_, logoDir_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    std::thread th_;
    bool quit_ = false;
    std::atomic<bool> busy_{false};
    std::atomic<uint64_t> version_{1};
    std::vector<Station> results_, favs_, recent_;
    std::vector<Country> countries_;
    bool countriesAsked_ = false;
    std::string error_, country_, server_;
    uint64_t query_ = 0;  // (a newer question makes older answers moot)
    Station cur_;
    std::string title_;
    std::atomic<uint64_t> playerId_{0};
    int64_t pausedAt_ = 0;
    std::vector<std::string> logosAsked_;
    struct Fetch;
    std::shared_ptr<Fetch> fetch_;
};

Radio& R();

}  // namespace radio
