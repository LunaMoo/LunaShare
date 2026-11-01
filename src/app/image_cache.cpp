#include "image_cache.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "gl_funcs.h"
#include "platform.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_FAILURE_USERMSG
#include "stb_image.h"

ImageCache& Images() {
    static ImageCache c;
    return c;
}

bool ImageCache::IsImagePath(const std::string& path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string e = path.substr(dot + 1);
    std::transform(e.begin(), e.end(), e.begin(), ::tolower);
    return e == "jpg" || e == "jpeg" || e == "png" || e == "bmp" || e == "gif" || e == "tga" || e == "psd" || e == "pnm" ||
           e == "ppm" || e == "pgm" || e == "hdr" || e == "jfif";
}

ImageCache::ImageCache() {
    for (int i = 0; i < 2; i++) threads_.emplace_back([this] { Worker(); });
}

ImageCache::~ImageCache() { Shutdown(); }

void ImageCache::Shutdown() {
    {
        std::lock_guard<std::mutex> l(m_);
        if (stop_) return;
        stop_ = true;
        jobs_.clear();
    }
    cv_.notify_all();
    for (auto& t : threads_)
        if (t.joinable()) t.join();
    for (auto& [k, v] : cache_)
        if (v.tex && glf::DeleteTextures) glf::DeleteTextures(1, &v.tex);
    cache_.clear();
}

const ImageTex& ImageCache::Get(const std::string& path, bool full) {
    std::string key = (full ? "F|" : "T|") + path;
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        it->second.lastUsed = frame_;
        return it->second;
    }
    ImageTex& t = cache_[key];
    t.lastUsed = frame_;
    {
        std::lock_guard<std::mutex> l(m_);
        Job j{key, path, full ? maxTex_ : 512};
        if (full) jobs_.push_front(j);  // the viewer is waiting for this one
        else jobs_.push_back(j);
    }
    cv_.notify_one();
    return t;
}

// ---- EXIF orientation (JPEG APP1) ------------------------------------------------
static int ExifOrientation(const unsigned char* d, size_t n) {
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return 1;
    size_t p = 2;
    while (p + 4 < n) {
        if (d[p] != 0xFF) return 1;
        unsigned char marker = d[p + 1];
        size_t len = ((size_t)d[p + 2] << 8) | d[p + 3];
        if (marker == 0xDA || marker == 0xD9) return 1;  // start of scan: no EXIF before it
        if (marker == 0xE1 && p + 4 + len <= n && len > 14 && memcmp(d + p + 4, "Exif\0\0", 6) == 0) {
            const unsigned char* t = d + p + 10;
            size_t tn = len - 8;
            bool le = t[0] == 'I';
            auto u16 = [&](size_t o) -> unsigned { return o + 2 > tn ? 0 : le ? (t[o] | t[o + 1] << 8) : (t[o] << 8 | t[o + 1]); };
            auto u32 = [&](size_t o) -> size_t {
                if (o + 4 > tn) return 0;
                return le ? ((size_t)t[o] | (size_t)t[o + 1] << 8 | (size_t)t[o + 2] << 16 | (size_t)t[o + 3] << 24)
                          : ((size_t)t[o] << 24 | (size_t)t[o + 1] << 16 | (size_t)t[o + 2] << 8 | (size_t)t[o + 3]);
            };
            size_t ifd = u32(4);
            unsigned count = u16(ifd);
            for (unsigned i = 0; i < count; i++) {
                size_t e = ifd + 2 + i * 12;
                if (u16(e) == 0x0112) {
                    unsigned v = u16(e + 8);
                    return v >= 1 && v <= 8 ? (int)v : 1;
                }
            }
            return 1;
        }
        p += 2 + len;
    }
    return 1;
}

static std::vector<unsigned char> Downscale(const unsigned char* src, int w, int h, int nw, int nh) {
    std::vector<unsigned char> out((size_t)nw * nh * 4);
    for (int y = 0; y < nh; y++) {
        int y0 = (int)((int64_t)y * h / nh), y1 = std::max(y0 + 1, (int)((int64_t)(y + 1) * h / nh));
        for (int x = 0; x < nw; x++) {
            int x0 = (int)((int64_t)x * w / nw), x1 = std::max(x0 + 1, (int)((int64_t)(x + 1) * w / nw));
            unsigned acc[4] = {0, 0, 0, 0};
            int cnt = 0;
            int stepY = std::max(1, (y1 - y0) / 4), stepX = std::max(1, (x1 - x0) / 4);  // sample at most 4x4 per pixel
            for (int sy = y0; sy < y1; sy += stepY)
                for (int sx = x0; sx < x1; sx += stepX) {
                    const unsigned char* s = src + ((size_t)sy * w + sx) * 4;
                    acc[0] += s[0]; acc[1] += s[1]; acc[2] += s[2]; acc[3] += s[3];
                    cnt++;
                }
            unsigned char* o = &out[((size_t)y * nw + x) * 4];
            for (int c = 0; c < 4; c++) o[c] = (unsigned char)(acc[c] / (unsigned)cnt);
        }
    }
    return out;
}

static std::vector<unsigned char> Orient(const std::vector<unsigned char>& s, int w, int h, int o, int& ow, int& oh) {
    bool swap = o >= 5;
    ow = swap ? h : w;
    oh = swap ? w : h;
    std::vector<unsigned char> d((size_t)ow * oh * 4);
    for (int dy = 0; dy < oh; dy++)
        for (int dx = 0; dx < ow; dx++) {
            int sx = dx, sy = dy;
            switch (o) {
                case 2: sx = w - 1 - dx; sy = dy; break;
                case 3: sx = w - 1 - dx; sy = h - 1 - dy; break;
                case 4: sx = dx; sy = h - 1 - dy; break;
                case 5: sx = dy; sy = dx; break;
                case 6: sx = dy; sy = h - 1 - dx; break;
                case 7: sx = w - 1 - dy; sy = h - 1 - dx; break;
                case 8: sx = w - 1 - dy; sy = dx; break;
                default: break;
            }
            memcpy(&d[((size_t)dy * ow + dx) * 4], &s[((size_t)sy * w + sx) * 4], 4);
        }
    return d;
}

void ImageCache::Worker() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> l(m_);
            cv_.wait(l, [&] { return stop_ || !jobs_.empty(); });
            if (stop_) return;
            job = jobs_.front();
            jobs_.pop_front();
        }
        Result r;
        r.key = job.key;
        std::vector<unsigned char> file;
        if (FILE* f = ls::OpenFile(job.path, "rb")) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 0 && sz < (512l << 20)) {
                file.resize((size_t)sz);
                if (fread(file.data(), 1, file.size(), f) != file.size()) file.clear();
            }
            fclose(f);
        }
        int w = 0, h = 0, ch = 0;
        unsigned char* px = file.empty() ? nullptr : stbi_load_from_memory(file.data(), (int)file.size(), &w, &h, &ch, 4);
        if (px && w > 0 && h > 0) {
            r.srcW = w;
            r.srcH = h;
            int ms = job.maxSide;
            int nw = w, nh = h;
            if (std::max(w, h) > ms) {
                double s = (double)ms / std::max(w, h);
                nw = std::max(1, (int)(w * s));
                nh = std::max(1, (int)(h * s));
            }
            std::vector<unsigned char> img = (nw != w || nh != h) ? Downscale(px, w, h, nw, nh) : std::vector<unsigned char>(px, px + (size_t)w * h * 4);
            stbi_image_free(px);
            int o = ExifOrientation(file.data(), file.size());
            if (o != 1) {
                int ow, oh;
                img = Orient(img, nw, nh, o, ow, oh);
                nw = ow;
                nh = oh;
                if (o >= 5) std::swap(r.srcW, r.srcH);
            }
            r.rgba = std::move(img);
            r.w = nw;
            r.h = nh;
            r.ok = true;
        }
        std::lock_guard<std::mutex> l(m_);
        done_.push_back(std::move(r));
    }
}

void ImageCache::Update() {
    frame_++;
    if (frame_ == 1 && glf::GetIntegerv) {
        int mt = 0;
        glf::GetIntegerv(glf::MAX_TEXTURE_SIZE, &mt);
        if (mt >= 1024) maxTex_ = std::min(mt, 8192);
    }
    std::vector<Result> done;
    {
        std::lock_guard<std::mutex> l(m_);
        done.swap(done_);
    }
    for (auto& r : done) {
        auto it = cache_.find(r.key);
        if (it == cache_.end()) continue;  // evicted meanwhile
        ImageTex& t = it->second;
        if (!r.ok || !glf::GenTextures) {
            t.state = ImageTex::Failed;
            continue;
        }
        int prev = 0;
        glf::GetIntegerv(glf::TEXTURE_BINDING_2D, &prev);
        glf::GenTextures(1, &t.tex);
        glf::BindTexture(glf::TEXTURE_2D, t.tex);
        glf::TexParameteri(glf::TEXTURE_2D, glf::TEXTURE_MIN_FILTER, glf::LINEAR);
        glf::TexParameteri(glf::TEXTURE_2D, glf::TEXTURE_MAG_FILTER, glf::LINEAR);
        glf::TexParameteri(glf::TEXTURE_2D, glf::TEXTURE_WRAP_S, glf::CLAMP_TO_EDGE);
        glf::TexParameteri(glf::TEXTURE_2D, glf::TEXTURE_WRAP_T, glf::CLAMP_TO_EDGE);
        glf::PixelStorei(glf::UNPACK_ALIGNMENT, 1);
        glf::TexImage2D(glf::TEXTURE_2D, 0, (int)glf::RGBA, r.w, r.h, 0, glf::RGBA, glf::UNSIGNED_BYTE, r.rgba.data());
        glf::BindTexture(glf::TEXTURE_2D, (unsigned)prev);
        t.w = r.w;
        t.h = r.h;
        t.srcW = r.srcW;
        t.srcH = r.srcH;
        t.state = ImageTex::Ready;
    }
    // Evict: thumbnails unused for ~30 s beyond the newest 300, full images unused for a few frames beyond 4.
    if (frame_ % 120 == 0) {
        std::vector<std::pair<uint64_t, std::string>> thumbs, fulls;
        for (auto& [k, v] : cache_) (k[0] == 'F' ? fulls : thumbs).push_back({v.lastUsed, k});
        auto evict = [&](std::vector<std::pair<uint64_t, std::string>>& v, size_t keep, uint64_t minAge) {
            if (v.size() <= keep) return;
            std::sort(v.begin(), v.end());
            for (size_t i = 0; i + keep < v.size(); i++) {
                if (frame_ - v[i].first < minAge) break;
                auto it = cache_.find(v[i].second);
                if (it->second.state == ImageTex::Loading) continue;
                if (it->second.tex) glf::DeleteTextures(1, &it->second.tex);
                cache_.erase(it);
            }
        };
        evict(thumbs, 300, 1800);
        evict(fulls, 4, 30);
    }
}
