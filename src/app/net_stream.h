// LunaShare - reading a URL as it comes (internet radio): http and https, redirects, a response read in pieces.
// Desktops: WinHTTP on Windows, sockets + OpenSSL elsewhere; Android: the platform's HttpURLConnection through the
// bridge. Icecast / Shoutcast song titles ("ICY" metadata woven into the stream) are taken out and reported.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ls {

class NetStream {
public:
    virtual ~NetStream() = default;
    // GET; follows redirects. icy: ask for song titles (the stream then carries them; Read() takes them out)
    virtual bool Open(const std::string& url, bool icy, std::string* err) = 0;
    // blocks until bytes come: how many, 0 at the end, < 0 on an error (or cancelled)
    virtual int Read(uint8_t* buf, int len) = 0;
    int status = 0;
    std::map<std::string, std::string> headers;  // names in lower case
    std::string finalUrl;                       // after redirects
    std::atomic<bool>* cancel = nullptr;         // set from another thread: reads give up
    std::string Header(const std::string& name) const {
        auto it = headers.find(name);
        return it == headers.end() ? std::string() : it->second;
    }
};

std::unique_ptr<NetStream> NewNetStream();

// A stream with its "icy-metaint" titles taken out: what the decoder reads is only sound. onTitle gets each title
// ("Artist - Song") as it comes.
class IcyStream {
public:
    explicit IcyStream(std::unique_ptr<NetStream> s);
    int Read(uint8_t* buf, int len);
    std::function<void(const std::string&)> onTitle;
    NetStream& Net() { return *s_; }

private:
    std::unique_ptr<NetStream> s_;
    int metaint_ = 0, left_ = 0;
};

// the whole body (directories, playlists); false on an error or when it's larger than maxBytes
bool HttpGetAll(const std::string& url, std::string& body, size_t maxBytes = 4u << 20, std::string* contentType = nullptr);

// "a/b/c.m3u8" relative to a base URL
std::string ResolveUrl(const std::string& base, const std::string& ref);

}  // namespace ls
