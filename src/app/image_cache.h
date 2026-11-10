// Asynchronous image decoding (stb_image, EXIF orientation aware) + GL texture cache for chat thumbnails
// and the full-screen picture viewer.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"

struct ImageTex {
    enum State { Loading, Ready, Failed };
    State state = Loading;
    unsigned int tex = 0;
    int w = 0, h = 0;          // texture size (possibly downscaled)
    int srcW = 0, srcH = 0;    // original size
    uint64_t lastUsed = 0;
    ImTextureID Id() const { return (ImTextureID)(intptr_t)tex; }
};

class ImageCache {
public:
    ImageCache();
    ~ImageCache();
    enum class Size { Tile, Thumb, Full };
    // Thumb: <= 512 px. Tile: <= 256 px, uses the JPEG's embedded EXIF preview when present (very fast, for
    // file-picker galleries). Full: up to the GL texture limit. Never blocks; returns Loading until decoded.
    const ImageTex& Get(const std::string& path, Size size);
    const ImageTex& Get(const std::string& path, bool full) { return Get(path, full ? Size::Full : Size::Thumb); }
    void Prefetch(const std::string& path, bool full) { (void)Get(path, full); }
    // Call once per frame on the GL thread: uploads finished decodes, evicts old textures.
    void Update();
    void Shutdown();
    static bool IsImagePath(const std::string& path);

private:
    struct Job {
        std::string key, path;
        int maxSide;
        bool preferExif = false;
        uint64_t frame = 0;
    };
    struct Result {
        std::string key;
        std::vector<unsigned char> rgba;
        int w = 0, h = 0, srcW = 0, srcH = 0;
        bool ok = false;
        bool cancelled = false;
    };
    void Worker();
    std::map<std::string, ImageTex> cache_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::vector<Result> done_;
    std::vector<std::thread> threads_;
    bool stop_ = false;
    uint64_t frame_ = 0;
    std::atomic<uint64_t> frameAtomic_{0};
    int maxTex_ = 4096;
};

ImageCache& Images();

// When a photo was taken (EXIF DateTimeOriginal, else DateTime; local time, ms since 1970), 0 = not known. Reads only
// the start of the file.
int64_t PhotoDateMs(const std::string& path);

// Synchronous decode of a picture file to RGBA, EXIF orientation applied, longest side <= maxSide.
bool DecodeImageFile(const std::string& path, int maxSide, std::vector<unsigned char>& rgba, int& w, int& h);
