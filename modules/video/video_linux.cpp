// Video backend for Linux: cameras through V4L2, H.264 encoding (x264) and H.264 / HEVC / AV1 decoding through the
// bundled FFmpeg (libavcodec, loaded at run time from the module's folder), and "LunaShare Camera" for other apps
// through the v4l2loopback kernel module.
// SPDX-License-Identifier: GPL-3.0-only
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#include "screen_grab.h"
#include "scale.h"
#include "video_backend.h"

namespace lsv {
namespace {

int Xioctl(int fd, unsigned long req, void* arg) {
    int r;
    do r = ioctl(fd, req, arg);
    while (r < 0 && errno == EINTR);
    return r;
}

std::string ModuleDir() {
    Dl_info info{};
    if (dladdr((void*)&ModuleDir, &info) && info.dli_fname) {
        std::string p = info.dli_fname;
        size_t s = p.rfind('/');
        if (s != std::string::npos) return p.substr(0, s);
    }
    return ".";
}

std::vector<std::string> VideoNodes() {
    std::vector<std::string> v;
    if (DIR* d = opendir("/dev")) {
        while (dirent* e = readdir(d))
            if (strncmp(e->d_name, "video", 5) == 0) v.push_back(std::string("/dev/") + e->d_name);
        closedir(d);
    }
    std::sort(v.begin(), v.end(), [](const std::string& a, const std::string& b) {
        return atoi(a.c_str() + 10) < atoi(b.c_str() + 10);
    });
    return v;
}

bool IsLoopback(const v4l2_capability& cap) { return strstr((const char*)cap.driver, "loopback") != nullptr; }

// ======================================================================== virtual camera (v4l2loopback)
// Other apps (browsers, Zoom, OBS, ...) see the received video as an ordinary camera. Needs the v4l2loopback kernel
// module (package v4l2loopback-dkms / akmod-v4l2loopback); LunaShare loads it once with pkexec when it isn't loaded.
class LoopbackCamera {
public:
    static constexpr int kW = 1920, kH = 1080;

    ~LoopbackCamera() { Disable(); }

    static std::string FindDevice() {
        for (auto& n : VideoNodes()) {
            int fd = open(n.c_str(), O_RDWR | O_NONBLOCK);
            if (fd < 0) continue;
            v4l2_capability cap{};
            bool ok = Xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 && IsLoopback(cap);
            close(fd);
            if (ok) return n;
        }
        return "";
    }
    static bool ModuleInstalled() {
        utsname u{};
        if (uname(&u) != 0) return false;
        std::string cmd = std::string("modinfo -n v4l2loopback >/dev/null 2>&1");
        return system(cmd.c_str()) == 0;
    }
    bool Available() { return !FindDevice().empty() || ModuleInstalled(); }

    bool Enable(bool on, std::string* err) {
        if (!on) {
            Disable();
            return true;
        }
        if (thread_.joinable()) return true;
        std::string dev = FindDevice();
        if (dev.empty()) {
            if (!ModuleInstalled()) {
                *err = "install the v4l2loopback kernel module first (Debian/Ubuntu: v4l2loopback-dkms, Fedora: akmod-v4l2loopback, Arch: v4l2loopback-dkms)";
                return false;
            }
            // one-time per boot: load it (asks for the password through polkit)
            int r = system("pkexec modprobe v4l2loopback devices=1 video_nr=42 card_label=\"LunaShare Camera\" exclusive_caps=1 >/dev/null 2>&1");
            for (int i = 0; i < 20 && dev.empty(); i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                dev = FindDevice();
            }
            if (dev.empty()) {
                *err = r != 0 ? "loading v4l2loopback needs administrator permission (it was declined or pkexec is missing)"
                              : "v4l2loopback loaded, but no loopback device appeared";
                return false;
            }
        }
        fd_ = open(dev.c_str(), O_WRONLY);
        if (fd_ < 0) {
            *err = "cannot open " + dev + ": " + strerror(errno);
            return false;
        }
        v4l2_format f{};
        f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        f.fmt.pix.width = kW;
        f.fmt.pix.height = kH;
        f.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        f.fmt.pix.field = V4L2_FIELD_NONE;
        f.fmt.pix.bytesperline = kW * 2;
        f.fmt.pix.sizeimage = kW * kH * 2;
        f.fmt.pix.colorspace = V4L2_COLORSPACE_REC709;
        if (Xioctl(fd_, VIDIOC_S_FMT, &f) < 0) {
            *err = std::string("the loopback device refused the picture format: ") + strerror(errno);
            close(fd_);
            fd_ = -1;
            return false;
        }
        frame_.assign((size_t)kW * kH * 2, 0);
        Dark();
        stop_ = false;
        thread_ = std::thread([this] { Loop(); });
        return true;
    }

    void Disable() {
        {
            std::lock_guard<std::mutex> l(m_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
    }

    // the received picture, any thread: scaled into 1920x1080 (letterboxed), turned upright
    void Push(const LsvImage& im) {
        if (fd_ < 0 || !im.y || im.width <= 0 || im.height <= 0) return;
        int rot = ((im.rotation % 360) + 360) % 360;
        int uw = rot % 180 ? im.height : im.width, uh = rot % 180 ? im.width : im.height;  // upright size
        double k = std::min((double)kW / uw, (double)kH / uh);
        int ow = std::max(2, (int)(uw * k) & ~1), oh = std::max(2, (int)(uh * k) & ~1);
        int ox = ((kW - ow) / 2) & ~1, oy = (kH - oh) / 2;
        auto src = [&](int ux, int uy, int& sx, int& sy) {  // upright -> source coordinates
            switch (rot) {
                case 90: sx = uy, sy = im.height - 1 - ux; break;
                case 180: sx = im.width - 1 - ux, sy = im.height - 1 - uy; break;
                case 270: sx = im.width - 1 - uy, sy = ux; break;
                default: sx = ux, sy = uy; break;
            }
            sx = std::clamp(sx, 0, im.width - 1);
            sy = std::clamp(sy, 0, im.height - 1);
        };
        std::lock_guard<std::mutex> l(fm_);
        Dark();
        int ps = im.uv_pixel_stride;
        for (int y = 0; y < oh; y++) {
            int uy = (int)((int64_t)y * uh / oh);
            uint8_t* row = &frame_[((size_t)(y + oy) * kW + ox) * 2];
            for (int x = 0; x < ow; x += 2) {
                int sx, sy, sx2, sy2;
                src((int)((int64_t)x * uw / ow), uy, sx, sy);
                src((int)((int64_t)(x + 1) * uw / ow), uy, sx2, sy2);
                size_t ci = (size_t)(sy >> 1) * im.uv_stride + (size_t)(sx >> 1) * ps;
                row[x * 2] = im.y[(size_t)sy * im.y_stride + sx];
                row[x * 2 + 1] = im.u[ci];
                row[x * 2 + 2] = im.y[(size_t)sy2 * im.y_stride + sx2];
                row[x * 2 + 3] = im.v[ci];
            }
        }
        lastPush_ = std::chrono::steady_clock::now();
        havePicture_ = true;
    }

private:
    void Dark() {
        for (size_t i = 0; i < frame_.size(); i += 2) {
            frame_[i] = 16;
            frame_[i + 1] = 128;
        }
    }
    void Loop() {
        // a steady 30 fps for the apps reading it; a dark picture when nothing has come in for a second
        auto next = std::chrono::steady_clock::now();
        std::vector<uint8_t> out;
        for (;;) {
            {
                std::unique_lock<std::mutex> l(m_);
                if (cv_.wait_until(l, next, [&] { return stop_; })) break;
            }
            next += std::chrono::milliseconds(33);
            {
                std::lock_guard<std::mutex> l(fm_);
                if (havePicture_ && std::chrono::steady_clock::now() - lastPush_ > std::chrono::seconds(1)) {
                    Dark();
                    havePicture_ = false;
                }
                out = frame_;
            }
            if (write(fd_, out.data(), out.size()) < 0 && errno != EAGAIN) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
    }
    int fd_ = -1;
    std::thread thread_;
    std::mutex m_, fm_;
    std::condition_variable cv_;
    bool stop_ = false, havePicture_ = false;
    std::vector<uint8_t> frame_;
    std::chrono::steady_clock::time_point lastPush_;
};

// ======================================================================== backend
struct Mode {
    uint32_t fourcc = 0;
    int w = 0, h = 0;
    double fps = 0;
};

class LinuxBackend : public Backend {
public:
    explicit LinuxBackend(Sink* s) : sink_(s) {
        std::string err;
        std::vector<std::string> dirs = {ModuleDir()};
        if (const char* d = getenv("LUNASHARE_FFMPEG_LIB")) dirs.insert(dirs.begin(), d);
        ffOk_ = FfDecoder::Load(dirs, &err);
        if (!ffOk_) sink_->Log("FFmpeg not found (" + err + "): no video");
    }
    ~LinuxBackend() override {
        vcam_.Disable();
        StopCamera();
        StopDecoder();
    }

    std::vector<LsvCamera> Cameras() override {
        std::vector<LsvCamera> out;
        if (getenv("LUNASHARE_TEST_CAMERA")) {  // development: a moving test picture through the real encoder
            LsvCamera c{};
            snprintf(c.id, sizeof(c.id), "test");
            snprintf(c.name, sizeof(c.name), "Test pattern");
            c.facing = LSV_FACING_FRONT;
            out.push_back(c);
        }
        for (auto& n : VideoNodes()) {
            int fd = open(n.c_str(), O_RDWR | O_NONBLOCK);
            if (fd < 0) continue;
            v4l2_capability cap{};
            bool ok = Xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0;
            uint32_t caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS ? cap.device_caps : cap.capabilities;
            close(fd);
            // capture devices only (UVC also exposes metadata nodes), and not our own virtual camera
            if (!ok || !(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING) || IsLoopback(cap)) continue;
            LsvCamera c{};
            snprintf(c.id, sizeof(c.id), "%s", n.c_str());
            snprintf(c.name, sizeof(c.name), "%s", (const char*)cap.card);
            std::string lower = c.name;
            for (auto& ch : lower) ch = (char)tolower((unsigned char)ch);
            c.facing = lower.find("front") != std::string::npos || lower.find("integrated") != std::string::npos ? LSV_FACING_FRONT
                                                                                                                   : LSV_FACING_EXTERNAL;
            out.push_back(c);
        }
        if (ScreenGrab::Available()) {  // the screen: shared like a camera
            LsvCamera c{};
            snprintf(c.id, sizeof(c.id), "screen");
            snprintf(c.name, sizeof(c.name), "Screen (share it)");
            c.facing = LSV_FACING_EXTERNAL;
            out.push_back(c);
        }
        return out;
    }

    bool StartCamera(const std::string& id, const LsvSettings& s, int codec, std::string* err) override {
        StopCamera();
        if (codec != kH264) {
            if (err) *err = "this PC only sends H.264";
            return false;
        }
        if (!ffOk_) {
            if (err) *err = "FFmpeg libraries are missing next to the video module";
            return false;
        }
        s_ = s;
        bitrate_ = s.bitrate;
        boxW_ = boxH_ = encFps_ = 0;
        gate_ = FrameGate();
        std::string dev = id;
        if (dev.empty()) {
            for (auto& c : Cameras())
                if (dev.empty() && strcmp(c.id, "screen") != 0) dev = c.id;  // (the screen only when it's picked)
            if (dev.empty()) {
                if (err) *err = "no camera found";
                return false;
            }
        }
        if (!OpenCamera(dev, err)) {
            CloseCamera();
            return false;
        }
        run_ = true;
        capThread_ = std::thread([this] { CaptureLoop(); });
        encThread_ = std::thread([this] { EncodeLoop(); });
        return true;
    }

    void StopCamera() override {
        run_ = false;
        encCv_.notify_all();
        if (capThread_.joinable()) capThread_.join();
        if (encThread_.joinable()) encThread_.join();
        CloseCamera();
    }

    void SetBitrate(int bps) override { bitrate_ = bps; }
    void RequestKeyframe() override { keyReq_ = true; }
    // smaller pictures: scaled down in front of the encoder (which is reopened at the new size), camera untouched
    bool ScalesDown() const override { return true; }
    bool Rescale(const LsvSettings& s) override {
        if (!run_) return false;
        boxW_ = s.width;
        boxH_ = s.height;
        encFps_ = s.fps;
        bitrate_ = s.bitrate;
        keyReq_ = true;
        encCv_.notify_one();
        return true;
    }

    // ---------------------------------------------------------------- camera controls (V4L2)
    std::vector<LsvControl> Controls() override {
        std::lock_guard<std::mutex> l(ctlM_);
        std::vector<LsvControl> out;
        if (fd_ < 0) return out;
        auto q = [&](uint32_t cid, v4l2_queryctrl* qc) {
            memset(qc, 0, sizeof(*qc));
            qc->id = cid;
            return Xioctl(fd_, VIDIOC_QUERYCTRL, qc) == 0 && !(qc->flags & V4L2_CTRL_FLAG_DISABLED);
        };
        auto get = [&](uint32_t cid, int def) {
            v4l2_control c{cid, 0};
            return Xioctl(fd_, VIDIOC_G_CTRL, &c) == 0 ? c.value : def;
        };
        v4l2_queryctrl qc;
        if (q(V4L2_CID_ZOOM_ABSOLUTE, &qc) && qc.maximum > qc.minimum) {
            zoomMin_ = qc.minimum;
            zoomMax_ = qc.maximum;
            // shown as a ratio: minimum = 1x
            double maxRatio = std::max(1.0, (double)qc.maximum / std::max(1, qc.minimum));
            if (qc.minimum <= 0) maxRatio = 1.0 + (qc.maximum - qc.minimum) / 100.0;
            zoomMaxRatio_ = maxRatio;
            int v = get(V4L2_CID_ZOOM_ABSOLUTE, qc.minimum);
            out.push_back({LSV_CTL_ZOOM, 1.0f, (float)maxRatio, 0.1f, (float)ZoomRatio(v)});
        }
        if (q(V4L2_CID_EXPOSURE_AUTO, &qc)) {
            int v = get(V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_APERTURE_PRIORITY);
            out.push_back({LSV_CTL_AUTO_EXPOSURE, 0, 1, 1, v == V4L2_EXPOSURE_MANUAL ? 0.f : 1.f});
        }
        if (q(V4L2_CID_EXPOSURE_ABSOLUTE, &qc) && qc.maximum > qc.minimum)  // units of 100 us
            out.push_back({LSV_CTL_SHUTTER, qc.minimum / 10.0f, qc.maximum / 10.0f, 0, get(V4L2_CID_EXPOSURE_ABSOLUTE, qc.default_value) / 10.0f});
        if (q(V4L2_CID_FOCUS_AUTO, &qc)) out.push_back({LSV_CTL_AUTO_FOCUS, 0, 1, 1, (float)(get(V4L2_CID_FOCUS_AUTO, 1) ? 1 : 0)});
        if (q(V4L2_CID_FOCUS_ABSOLUTE, &qc) && qc.maximum > qc.minimum)
            out.push_back({LSV_CTL_FOCUS, (float)qc.minimum, (float)qc.maximum, (float)std::max(1, qc.step),
                           (float)get(V4L2_CID_FOCUS_ABSOLUTE, qc.default_value)});
        if (q(V4L2_CID_AUTO_WHITE_BALANCE, &qc) && q(V4L2_CID_WHITE_BALANCE_TEMPERATURE, &qc)) {
            int idx = get(V4L2_CID_AUTO_WHITE_BALANCE, 1) ? 0 : PresetFor(get(V4L2_CID_WHITE_BALANCE_TEMPERATURE, 5000));
            out.push_back({LSV_CTL_WHITE_BALANCE, 0, 5, 1, (float)idx});
        }
        if (q(V4L2_CID_BRIGHTNESS, &qc) && qc.maximum > qc.minimum)
            out.push_back({LSV_CTL_BRIGHTNESS, (float)qc.minimum, (float)qc.maximum, (float)std::max(1, qc.step),
                           (float)get(V4L2_CID_BRIGHTNESS, qc.default_value)});
        return out;
    }

    void SetControl(int id, float v) override {
        std::lock_guard<std::mutex> l(ctlM_);
        if (fd_ < 0) return;
        auto set = [&](uint32_t cid, int val) {
            v4l2_control c{cid, val};
            Xioctl(fd_, VIDIOC_S_CTRL, &c);
        };
        switch (id) {
            case LSV_CTL_ZOOM: set(V4L2_CID_ZOOM_ABSOLUTE, ZoomValue(v)); break;
            case LSV_CTL_AUTO_EXPOSURE: set(V4L2_CID_EXPOSURE_AUTO, v > 0.5f ? V4L2_EXPOSURE_APERTURE_PRIORITY : V4L2_EXPOSURE_MANUAL); break;
            case LSV_CTL_SHUTTER:
                set(V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_MANUAL);
                set(V4L2_CID_EXPOSURE_ABSOLUTE, (int)std::lround(v * 10));
                break;
            case LSV_CTL_AUTO_FOCUS: set(V4L2_CID_FOCUS_AUTO, v > 0.5f ? 1 : 0); break;
            case LSV_CTL_FOCUS:
                set(V4L2_CID_FOCUS_AUTO, 0);
                set(V4L2_CID_FOCUS_ABSOLUTE, (int)std::lround(v));
                break;
            case LSV_CTL_WHITE_BALANCE: {
                static const int kelvin[] = {0, 5500, 6500, 7500, 3200, 4000};  // Auto, Daylight, Cloudy, Shade, Tungsten, Fluorescent
                int i = std::clamp((int)std::lround(v), 0, 5);
                set(V4L2_CID_AUTO_WHITE_BALANCE, i == 0 ? 1 : 0);
                if (i) set(V4L2_CID_WHITE_BALANCE_TEMPERATURE, kelvin[i]);
                break;
            }
            case LSV_CTL_BRIGHTNESS: set(V4L2_CID_BRIGHTNESS, (int)std::lround(v)); break;
            default: break;
        }
    }

    // ---------------------------------------------------------------- decoding (core's decode thread)
    bool StartDecoder() override { return ffOk_; }
    unsigned EncodeCodecs() override { return CodecBit(kH264); }
    unsigned DecodeCodecs() override {
        if (!ffOk_) return CodecBit(kH264);
        unsigned m = CodecBit(kH264) | CodecBit(kHEVC);
        if (FfDecoder::HasSoftwareAv1()) m |= CodecBit(kAV1);
        return m;
    }
    void Decode(const uint8_t* d, size_t n, bool key, int, int, int codec) override {
        if (!dec_.IsOpen() || dec_.OpenCodec() != codec) {
            if (!key) {
                sink_->OnDecoderError();
                return;
            }
            std::string err;
            if (!dec_.Open(codec, decHw_, &err)) {
                sink_->Log("decoder: " + err);
                sink_->OnDecoderError();
                return;
            }
            sink_->Log("decoder: " + dec_.Name());
        }
        if (dec_.Decode(d, n, [&](const LsvImage& im) { sink_->OnDecoded(im); })) {
            decFails_ = 0;
            return;
        }
        sink_->OnDecoderError();
        if (dec_.Hardware() && ++decFails_ >= 3) {  // the GPU decoder can't do this stream: CPU from the next keyframe
            sink_->Log(dec_.Name() + " failed, decoding on the CPU");
            decHw_ = false;
            decFails_ = 0;
            dec_.Close();
        }
    }
    void StopDecoder() override { dec_.Close(); }
    std::string EncoderName() override {
        std::lock_guard<std::mutex> l(nameM_);
        return encName_;
    }
    std::string DecoderName() override { return dec_.IsOpen() ? dec_.Name() : ""; }

    bool VcamAvailable() override { return vcam_.Available(); }
    bool VcamEnable(bool on, std::string* err) override { return vcam_.Enable(on, err); }
    void VcamPush(const LsvImage& img) override { vcam_.Push(img); }

private:
    // ---------------------------------------------------------------- capture
    std::vector<Mode> Modes() {
        std::vector<Mode> v;
        for (uint32_t fi = 0;; fi++) {
            v4l2_fmtdesc fd{};
            fd.index = fi;
            fd.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (Xioctl(fd_, VIDIOC_ENUM_FMT, &fd) < 0) break;
            uint32_t fc = fd.pixelformat;
            if (fc != V4L2_PIX_FMT_MJPEG && fc != V4L2_PIX_FMT_YUYV && fc != V4L2_PIX_FMT_NV12 && fc != V4L2_PIX_FMT_YUV420) continue;
            for (uint32_t si = 0;; si++) {
                v4l2_frmsizeenum fs{};
                fs.index = si;
                fs.pixel_format = fc;
                if (Xioctl(fd_, VIDIOC_ENUM_FRAMESIZES, &fs) < 0) break;
                std::vector<std::pair<int, int>> sizes;
                if (fs.type == V4L2_FRMSIZE_TYPE_DISCRETE) sizes.push_back({(int)fs.discrete.width, (int)fs.discrete.height});
                else {  // stepwise: offer the usual sizes inside the range
                    for (auto wh : {std::pair<int, int>{640, 480}, {1280, 720}, {1920, 1080}, {3840, 2160}})
                        if (wh.first >= (int)fs.stepwise.min_width && wh.first <= (int)fs.stepwise.max_width &&
                            wh.second >= (int)fs.stepwise.min_height && wh.second <= (int)fs.stepwise.max_height)
                            sizes.push_back(wh);
                }
                for (auto [w, h] : sizes) {
                    double best = 0;
                    for (uint32_t ii = 0;; ii++) {
                        v4l2_frmivalenum fv{};
                        fv.index = ii;
                        fv.pixel_format = fc;
                        fv.width = (uint32_t)w;
                        fv.height = (uint32_t)h;
                        if (Xioctl(fd_, VIDIOC_ENUM_FRAMEINTERVALS, &fv) < 0) break;
                        if (fv.type == V4L2_FRMIVAL_TYPE_DISCRETE && fv.discrete.numerator)
                            best = std::max(best, (double)fv.discrete.denominator / fv.discrete.numerator);
                        else if (fv.stepwise.min.numerator)
                            best = std::max(best, (double)fv.stepwise.min.denominator / fv.stepwise.min.numerator);
                        if (fv.type != V4L2_FRMIVAL_TYPE_DISCRETE) break;
                    }
                    v.push_back({fc, w, h, best > 0 ? best : 30});
                    if (fs.type != V4L2_FRMSIZE_TYPE_DISCRETE) continue;
                }
                if (fs.type != V4L2_FRMSIZE_TYPE_DISCRETE) break;
            }
        }
        return v;
    }

    bool OpenCamera(const std::string& dev, std::string* err) {
        if (dev == "screen") {
            if (!grab_.Open(err)) return false;
            screen_ = true;
            w_ = grab_.Width();
            h_ = grab_.Height();
            fps_ = 10;  // (enough for a screen; the bitrate goes to sharpness)
            std::string e;
            if (!enc_.Open(w_, h_, fps_, std::max<int>(bitrate_, 1500000), &e)) {
                if (err) *err = "H.264 encoder: " + e;
                return false;
            }
            std::lock_guard<std::mutex> l(nameM_);
            encName_ = enc_.Name();
            return true;
        }
        screen_ = false;
        if (dev == "test") {
            test_ = true;
            w_ = s_.width & ~1;
            h_ = s_.height & ~1;
            fps_ = std::clamp(s_.fps, 1, 120);
            std::string e;
            if (!enc_.Open(w_, h_, fps_, bitrate_, &e)) {
                if (err) *err = "H.264 encoder: " + e;
                return false;
            }
            std::lock_guard<std::mutex> l(nameM_);
            encName_ = enc_.Name();
            return true;
        }
        test_ = false;
        int fd = open(dev.c_str(), O_RDWR | O_NONBLOCK);
        if (fd < 0) {
            if (err) *err = "cannot open " + dev + ": " + strerror(errno) + (errno == EACCES ? " (is your user in the 'video' group?)" : "");
            return false;
        }
        {
            std::lock_guard<std::mutex> l(ctlM_);
            fd_ = fd;
        }
        // the mode closest to what was asked (size first, then frame rate); compressed MJPEG when raw can't keep up
        auto modes = Modes();
        if (modes.empty()) {
            if (err) *err = "the camera offers no usable picture format";
            return false;
        }
        Mode best;
        double bestScore = 1e18;
        for (auto& m : modes) {
            double score = std::abs((double)m.h - s_.height) * 4 + std::abs((double)m.w - s_.width) +
                           (m.fps + 0.5 < s_.fps ? 1500 + (s_.fps - m.fps) * 20 : 0) + (m.h < s_.height ? 400 : 0) +
                           (m.fourcc == V4L2_PIX_FMT_MJPEG ? 30 : 0);  // raw is a little better when it's just as fast
            if (score < bestScore) bestScore = score, best = m;
        }
        v4l2_format f{};
        f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        f.fmt.pix.width = (uint32_t)best.w;
        f.fmt.pix.height = (uint32_t)best.h;
        f.fmt.pix.pixelformat = best.fourcc;
        f.fmt.pix.field = V4L2_FIELD_ANY;
        if (Xioctl(fd_, VIDIOC_S_FMT, &f) < 0) {
            if (err) *err = std::string("camera format: ") + strerror(errno);
            return false;
        }
        fourcc_ = f.fmt.pix.pixelformat;
        w_ = (int)f.fmt.pix.width;
        h_ = (int)f.fmt.pix.height;
        stride_ = (int)f.fmt.pix.bytesperline;
        v4l2_streamparm sp{};
        sp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        sp.parm.capture.timeperframe.numerator = 1;
        sp.parm.capture.timeperframe.denominator = (uint32_t)std::max(1, std::min(s_.fps, (int)std::lround(best.fps)));
        Xioctl(fd_, VIDIOC_S_PARM, &sp);
        fps_ = sp.parm.capture.timeperframe.numerator ? (int)std::lround((double)sp.parm.capture.timeperframe.denominator /
                                                                         sp.parm.capture.timeperframe.numerator)
                                                      : s_.fps;
        fps_ = std::clamp(fps_, 1, 240);
        // buffers
        v4l2_requestbuffers rb{};
        rb.count = 4;
        rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        rb.memory = V4L2_MEMORY_MMAP;
        if (Xioctl(fd_, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2) {
            if (err) *err = std::string("camera buffers: ") + strerror(errno);
            return false;
        }
        for (uint32_t i = 0; i < rb.count; i++) {
            v4l2_buffer b{};
            b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            b.memory = V4L2_MEMORY_MMAP;
            b.index = i;
            if (Xioctl(fd_, VIDIOC_QUERYBUF, &b) < 0) return false;
            void* p = mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, b.m.offset);
            if (p == MAP_FAILED) {
                if (err) *err = "camera mmap failed";
                return false;
            }
            bufs_.push_back({p, b.length});
            Xioctl(fd_, VIDIOC_QBUF, &b);
        }
        v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (Xioctl(fd_, VIDIOC_STREAMON, &t) < 0) {
            if (err) *err = std::string("camera start: ") + strerror(errno) + (errno == EBUSY ? " (used by another app?)" : "");
            return false;
        }
        streaming_ = true;
        if (fourcc_ == V4L2_PIX_FMT_MJPEG) {
            std::string e;
            if (!mjpeg_.Open(kMJPEG, false, &e)) {
                if (err) *err = "MJPEG decoder: " + e;
                return false;
            }
        }
        // the encoder works at the camera's size (even numbers)
        std::string e;
        if (!enc_.Open(w_ & ~1, h_ & ~1, fps_, bitrate_, &e)) {
            if (err) *err = "H.264 encoder: " + e;
            return false;
        }
        {
            std::lock_guard<std::mutex> l(nameM_);
            encName_ = enc_.Name();
        }
        char fc[5] = {(char)(fourcc_ & 255), (char)((fourcc_ >> 8) & 255), (char)((fourcc_ >> 16) & 255), (char)(fourcc_ >> 24), 0};
        sink_->Log(dev + ": " + std::to_string(w_) + "x" + std::to_string(h_) + " " + fc + " @" + std::to_string(fps_) + " fps, " + enc_.Name());
        return true;
    }

    void CloseCamera() {
        if (screen_) {
            grab_.Close();
            screen_ = false;
        }
        if (streaming_) {
            v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            Xioctl(fd_, VIDIOC_STREAMOFF, &t);
            streaming_ = false;
        }
        for (auto& b : bufs_) munmap(b.first, b.second);
        bufs_.clear();
        std::lock_guard<std::mutex> l(ctlM_);
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
        enc_.Close();
        mjpeg_.Close();
    }

    // camera bytes -> I420 in cur_ (tight)
    bool ToI420(const uint8_t* p, size_t len, std::vector<uint8_t>& out) {
        int W = w_ & ~1, H = h_ & ~1;
        out.resize((size_t)W * H * 3 / 2);
        uint8_t* Y = out.data();
        uint8_t* U = Y + (size_t)W * H;
        uint8_t* V = U + (size_t)W * H / 4;
        if (fourcc_ == V4L2_PIX_FMT_YUYV) {
            int st = stride_ ? stride_ : w_ * 2;
            for (int y = 0; y < H; y++) {
                const uint8_t* r = p + (size_t)y * st;
                uint8_t* yr = Y + (size_t)y * W;
                for (int x = 0; x < W; x++) yr[x] = r[x * 2];
                if (y & 1) continue;
                const uint8_t* r2 = p + (size_t)std::min(y + 1, H - 1) * st;
                uint8_t* ur = U + (size_t)(y / 2) * (W / 2);
                uint8_t* vr = V + (size_t)(y / 2) * (W / 2);
                for (int x = 0; x < W / 2; x++) {
                    ur[x] = (uint8_t)((r[x * 4 + 1] + r2[x * 4 + 1] + 1) >> 1);
                    vr[x] = (uint8_t)((r[x * 4 + 3] + r2[x * 4 + 3] + 1) >> 1);
                }
            }
            return true;
        }
        if (fourcc_ == V4L2_PIX_FMT_NV12 || fourcc_ == V4L2_PIX_FMT_YUV420) {
            int st = stride_ ? stride_ : w_;
            for (int y = 0; y < H; y++) memcpy(Y + (size_t)y * W, p + (size_t)y * st, (size_t)W);
            const uint8_t* c = p + (size_t)st * h_;
            if (fourcc_ == V4L2_PIX_FMT_NV12) {
                for (int y = 0; y < H / 2; y++)
                    for (int x = 0; x < W / 2; x++) {
                        U[(size_t)y * (W / 2) + x] = c[(size_t)y * st + x * 2];
                        V[(size_t)y * (W / 2) + x] = c[(size_t)y * st + x * 2 + 1];
                    }
            } else {
                for (int y = 0; y < H / 2; y++) memcpy(U + (size_t)y * (W / 2), c + (size_t)y * (st / 2), (size_t)W / 2);
                const uint8_t* cv = c + (size_t)(st / 2) * (h_ / 2);
                for (int y = 0; y < H / 2; y++) memcpy(V + (size_t)y * (W / 2), cv + (size_t)y * (st / 2), (size_t)W / 2);
            }
            return true;
        }
        if (fourcc_ == V4L2_PIX_FMT_MJPEG) {
            bool got = false;
            mjpeg_.Decode(p, len, [&](const LsvImage& im) {
                int w = std::min(W, im.width & ~1), h = std::min(H, im.height & ~1);
                for (int y = 0; y < h; y++) memcpy(Y + (size_t)y * W, im.y + (size_t)y * im.y_stride, (size_t)w);
                for (int y = 0; y < h / 2; y++)
                    for (int x = 0; x < w / 2; x++) {
                        size_t ci = (size_t)y * im.uv_stride + (size_t)x * im.uv_pixel_stride;
                        U[(size_t)y * (W / 2) + x] = im.u[ci];
                        V[(size_t)y * (W / 2) + x] = im.v[ci];
                    }
                got = true;
            });
            return got;
        }
        return false;
    }

    void CaptureLoop() {
        std::vector<uint8_t> frame;
        int misses = 0;
        auto next = std::chrono::steady_clock::now();
        int n = 0;
        while (run_ && screen_) {  // the screen, ten times a second
            next += std::chrono::milliseconds(100);
            std::this_thread::sleep_until(next);
            if (!grab_.Grab(frame, 1)) {
                if (++misses > 50) {
                    sink_->OnCameraError("the screen can't be read");
                    return;
                }
                continue;
            }
            misses = 0;
            int W = w_, H = h_;
            LsvImage img{W, H, frame.data(), frame.data() + (size_t)W * H, frame.data() + (size_t)W * H * 5 / 4, W, W / 2, 1, 0, 0};
            sink_->OnLocalImage(img);  // (not mirrored: it's a screen)
            {
                std::lock_guard<std::mutex> l(encM_);
                pending_.swap(frame);
                havePending_ = true;
            }
            encCv_.notify_one();
        }
        while (run_ && test_) {
            next += std::chrono::microseconds(1000000 / fps_);
            std::this_thread::sleep_until(next);
            int W = w_, H = h_;
            frame.resize((size_t)W * H * 3 / 2);
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++) frame[(size_t)y * W + x] = (uint8_t)(((x / 40 + y / 40 + n / 4) & 1) ? 200 : 40);
            memset(&frame[(size_t)W * H], (uint8_t)(90 + n % 80), (size_t)W * H / 4);
            memset(&frame[(size_t)W * H * 5 / 4], (uint8_t)(170 - n % 80), (size_t)W * H / 4);
            n++;
            LsvImage img{W, H, frame.data(), frame.data() + (size_t)W * H, frame.data() + (size_t)W * H * 5 / 4, W, W / 2, 1, 0, 1};
            sink_->OnLocalImage(img);
            {
                std::lock_guard<std::mutex> l(encM_);
                pending_.swap(frame);
                havePending_ = true;
            }
            encCv_.notify_one();
        }
        while (run_) {
            pollfd pf{fd_, POLLIN, 0};
            int r = poll(&pf, 1, 500);
            if (r <= 0) {
                if (r == 0 && ++misses > 10) {  // 5 s without a picture
                    sink_->OnCameraError("the camera stopped sending pictures");
                    return;
                }
                continue;
            }
            misses = 0;
            v4l2_buffer b{};
            b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            b.memory = V4L2_MEMORY_MMAP;
            if (Xioctl(fd_, VIDIOC_DQBUF, &b) < 0) {
                if (errno == EAGAIN) continue;
                sink_->OnCameraError(std::string("camera: ") + strerror(errno));
                return;
            }
            bool ok = b.index < bufs_.size() && ToI420((const uint8_t*)bufs_[b.index].first, b.bytesused, frame);
            Xioctl(fd_, VIDIOC_QBUF, &b);
            if (!ok) continue;
            int W = w_ & ~1, H = h_ & ~1;
            LsvImage img{W, H, frame.data(), frame.data() + (size_t)W * H, frame.data() + (size_t)W * H * 5 / 4, W, W / 2, 1, 0, 1};
            sink_->OnLocalImage(img);  // mirrored self-view
            {
                // hand the newest picture to the encoder thread (an older one it hasn't taken yet is dropped)
                std::lock_guard<std::mutex> l(encM_);
                pending_.swap(frame);
                havePending_ = true;
            }
            encCv_.notify_one();
        }
    }

    void EncodeLoop() {
        std::vector<uint8_t> work;
        while (run_) {
            {
                std::unique_lock<std::mutex> l(encM_);
                encCv_.wait_for(l, std::chrono::milliseconds(200), [&] { return havePending_ || !run_; });
                if (!run_) break;
                if (!havePending_) continue;
                work.swap(pending_);
                havePending_ = false;
            }
            int W = w_ & ~1, H = h_ & ~1;
            if (work.size() < (size_t)W * H * 3 / 2) continue;
            int64_t nowMs = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (!gate_.Pass(screen_ ? 0 : (int)encFps_, nowMs)) continue;  // (a lower frame rate than the camera's)
            LsvImage img{W, H, work.data(), work.data() + (size_t)W * H, work.data() + (size_t)W * H * 5 / 4, W, W / 2, 1, 0, 0};
            int ew, eh;
            FitInBox(W, H, boxW_, boxH_, 2, ew, eh);
            if (ew != enc_.Width() || eh != enc_.Height()) {  // the size changed: a new encoder for it
                std::string e;
                int fps = encFps_ > 0 ? (int)encFps_ : fps_;
                if (!enc_.Open(ew, eh, fps, bitrate_, &e)) {
                    sink_->Log("encoder at " + std::to_string(ew) + "x" + std::to_string(eh) + " failed (" + e + "), back to full size");
                    boxW_ = boxH_ = 0;
                    ew = W;
                    eh = H;
                    if (!enc_.Open(W, H, fps_, bitrate_, &e)) {
                        sink_->OnCameraError("no working H.264 encoder: " + e);
                        return;
                    }
                }
                std::lock_guard<std::mutex> l(nameM_);
                encName_ = enc_.Name();
            }
            if (ew != W || eh != H) {
                ScaleYuv420(img, ew, eh, false, scaled_);
                img = LsvImage{ew, eh, scaled_.data(), scaled_.data() + (size_t)ew * eh, scaled_.data() + (size_t)ew * eh * 5 / 4, ew, ew / 2, 1, 0, 0};
                W = ew;
                H = eh;
            }
            enc_.SetBitrate(bitrate_);
            bool ok = enc_.Encode(img, keyReq_.exchange(false), [&](const uint8_t* d, size_t n, bool key) {
                EncodedFrame f;
                f.data = d;
                f.len = n;
                f.key = key;
                f.width = W;
                f.height = H;
                f.codec = kH264;
                sink_->OnEncoded(f);
            });
            if (!ok && enc_.Hardware() && ++encFails_ >= 3) {
                // a GPU encoder that stopped working: carry on with x264
                sink_->Log(enc_.Name() + " failed, switching to x264");
                std::string e;
                if (!enc_.FallBackToCpu(&e)) {
                    sink_->OnCameraError("no working H.264 encoder: " + e);
                    return;
                }
                encFails_ = 0;
                keyReq_ = true;
                std::lock_guard<std::mutex> l(nameM_);
                encName_ = enc_.Name();
            } else if (ok) {
                encFails_ = 0;
            }
        }
    }

    double ZoomRatio(int v) const {
        if (zoomMin_ > 0) return (double)v / zoomMin_;
        return 1.0 + (v - zoomMin_) / 100.0;
    }
    int ZoomValue(float ratio) const {
        int v = zoomMin_ > 0 ? (int)std::lround(ratio * zoomMin_) : zoomMin_ + (int)std::lround((ratio - 1.0) * 100.0);
        return std::clamp(v, zoomMin_, zoomMax_);
    }
    static int PresetFor(int kelvin) {
        static const int k[] = {0, 5500, 6500, 7500, 3200, 4000};
        int best = 1;
        for (int i = 1; i < 6; i++)
            if (std::abs(k[i] - kelvin) < std::abs(k[best] - kelvin)) best = i;
        return best;
    }

    Sink* sink_;
    bool ffOk_ = false;
    LsvSettings s_{};
    std::atomic<int> bitrate_{0};
    std::atomic<bool> run_{false}, keyReq_{false};
    std::thread capThread_, encThread_;
    std::mutex ctlM_, nameM_, encM_;
    std::condition_variable encCv_;
    std::vector<uint8_t> pending_, scaled_;
    bool havePending_ = false;
    std::atomic<int> boxW_{0}, boxH_{0}, encFps_{0};  // Rescale(): the box to fit the sent picture into, its frame rate
    FrameGate gate_;
    int fd_ = -1, w_ = 0, h_ = 0, stride_ = 0, fps_ = 30;
    uint32_t fourcc_ = 0;
    bool streaming_ = false, test_ = false, screen_ = false;
    ScreenGrab grab_;
    int encFails_ = 0, decFails_ = 0;
    bool decHw_ = true;
    std::vector<std::pair<void*, size_t>> bufs_;
    FfDecoder mjpeg_, dec_;
    FfEncoder enc_;
    std::string encName_;
    int zoomMin_ = 0, zoomMax_ = 0;
    double zoomMaxRatio_ = 1;
    LoopbackCamera vcam_;
};

}  // namespace

std::unique_ptr<Backend> CreateBackend(Sink* sink, const LsvHost*) { return std::make_unique<LinuxBackend>(sink); }

}  // namespace lsv
