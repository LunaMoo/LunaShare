// The screen as a video source ("share my screen" in a call): grabs the whole desktop, smaller when it's bigger than
// 1920 wide, as YUV for the encoder. Windows: GDI; Linux: X11 (libX11 loaded at run time - on Wayland only what
// XWayland shows). Header-only, used by the platform backends.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__) && !defined(__ANDROID__)
#include <dlfcn.h>
#endif

namespace lsv {

// BGRA (the screen) -> YUV 4:2:0 at dw x dh (nearest pixel when smaller); uvStep 1 = I420 (u, v planes), 2 = NV12 (u = v - 1)
inline void BgraToYuv(const uint8_t* src, int sw, int sh, int stride, int dw, int dh, uint8_t* yP, uint8_t* uP, uint8_t* vP, int uvStride,
                      int uvStep) {
    for (int y = 0; y < dh; y++) {
        const uint8_t* row = src + (size_t)std::min(sh - 1, y * sh / dh) * stride;
        uint8_t* yo = yP + (size_t)y * dw;
        for (int x = 0; x < dw; x++) {
            const uint8_t* p = row + (size_t)std::min(sw - 1, x * sw / dw) * 4;
            yo[x] = (uint8_t)((66 * p[2] + 129 * p[1] + 25 * p[0] + 128) / 256 + 16);
        }
    }
    for (int y = 0; y < dh / 2; y++) {
        const uint8_t* row = src + (size_t)std::min(sh - 1, (y * 2) * sh / dh) * stride;
        for (int x = 0; x < dw / 2; x++) {
            const uint8_t* p = row + (size_t)std::min(sw - 1, (x * 2) * sw / dw) * 4;
            int r = p[2], g = p[1], b = p[0];
            uP[(size_t)y * uvStride + (size_t)x * uvStep] = (uint8_t)((-38 * r - 74 * g + 112 * b + 128) / 256 + 128);
            vP[(size_t)y * uvStride + (size_t)x * uvStep] = (uint8_t)((112 * r - 94 * g - 18 * b + 128) / 256 + 128);
        }
    }
}

class ScreenGrab {
public:
    ~ScreenGrab() { Close(); }

    static bool Available() {
#ifdef _WIN32
        return true;
#elif defined(__linux__) && !defined(__ANDROID__)
        if (!getenv("DISPLAY")) return false;
        void* h = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
        if (h) dlclose(h);
        return h != nullptr;
#else
        return false;
#endif
    }

    // the size it's sent at (even, at most 1920 wide)
    int Width() const { return dw_; }
    int Height() const { return dh_; }

    bool Open(std::string* err) {
        Close();
#ifdef _WIN32
        sw_ = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        sh_ = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        sx_ = GetSystemMetrics(SM_XVIRTUALSCREEN);
        sy_ = GetSystemMetrics(SM_YVIRTUALSCREEN);
        if (sw_ <= 0 || sh_ <= 0) {
            if (err) *err = "no screen";
            return false;
        }
        screenDc_ = GetDC(nullptr);
        memDc_ = CreateCompatibleDC(screenDc_);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = sw_;
        bi.bmiHeader.biHeight = -sh_;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bmp_ = CreateDIBSection(memDc_, &bi, DIB_RGB_COLORS, (void**)&bits_, nullptr, 0);
        if (!bmp_ || !bits_) {
            if (err) *err = "cannot grab the screen";
            Close();
            return false;
        }
        old_ = SelectObject(memDc_, bmp_);
#elif defined(__linux__) && !defined(__ANDROID__)
        lib_ = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
        if (!lib_) {
            if (err) *err = "libX11 not found (sharing the screen needs X11)";
            return false;
        }
        auto sym = [&](const char* n) { return dlsym(lib_, n); };
        openDisplay_ = (void* (*)(const char*))sym("XOpenDisplay");
        closeDisplay_ = (int (*)(void*))sym("XCloseDisplay");
        defScreen_ = (int (*)(void*))sym("XDefaultScreen");
        rootWindow_ = (unsigned long (*)(void*, int))sym("XRootWindow");
        dispW_ = (int (*)(void*, int))sym("XDisplayWidth");
        dispH_ = (int (*)(void*, int))sym("XDisplayHeight");
        getImage_ = (XImg * (*)(void*, unsigned long, int, int, unsigned, unsigned, unsigned long, int)) sym("XGetImage");
        if (!openDisplay_ || !closeDisplay_ || !defScreen_ || !rootWindow_ || !dispW_ || !dispH_ || !getImage_) {
            if (err) *err = "libX11 is incomplete";
            Close();
            return false;
        }
        dpy_ = openDisplay_(nullptr);
        if (!dpy_) {
            if (err) *err = "cannot open the X display";
            Close();
            return false;
        }
        int scr = defScreen_(dpy_);
        root_ = rootWindow_(dpy_, scr);
        sw_ = dispW_(dpy_, scr);
        sh_ = dispH_(dpy_, scr);
#else
        if (err) *err = "sharing the screen isn't available here";
        return false;
#endif
        double k = sw_ > 1920 ? 1920.0 / sw_ : 1.0;
        dw_ = std::max(2, (int)(sw_ * k)) & ~1;
        dh_ = std::max(2, (int)(sh_ * k)) & ~1;
        return true;
    }

    // one picture; uvStep 1: out = I420, 2: out = NV12 (both tight, dw x dh)
    bool Grab(std::vector<uint8_t>& out, int uvStep) {
        out.resize((size_t)dw_ * dh_ * 3 / 2);
        uint8_t* y = out.data();
        uint8_t* uv = y + (size_t)dw_ * dh_;
#ifdef _WIN32
        if (!BitBlt(memDc_, 0, 0, sw_, sh_, screenDc_, sx_, sy_, SRCCOPY | CAPTUREBLT)) return false;
        GdiFlush();
        if (uvStep == 2) BgraToYuv(bits_, sw_, sh_, sw_ * 4, dw_, dh_, y, uv, uv + 1, dw_, 2);
        else BgraToYuv(bits_, sw_, sh_, sw_ * 4, dw_, dh_, y, uv, uv + (size_t)dw_ * dh_ / 4, dw_ / 2, 1);
        return true;
#elif defined(__linux__) && !defined(__ANDROID__)
        if (!dpy_) return false;
        XImg* im = getImage_(dpy_, root_, 0, 0, (unsigned)sw_, (unsigned)sh_, ~0UL, 2 /* ZPixmap */);
        if (!im) return false;
        bool ok = im->bits_per_pixel == 32 && im->data;
        if (ok) {
            if (uvStep == 2) BgraToYuv((const uint8_t*)im->data, sw_, sh_, im->bytes_per_line, dw_, dh_, y, uv, uv + 1, dw_, 2);
            else BgraToYuv((const uint8_t*)im->data, sw_, sh_, im->bytes_per_line, dw_, dh_, y, uv, uv + (size_t)dw_ * dh_ / 4, dw_ / 2, 1);
        }
        im->destroy(im);
        return ok;
#else
        (void)y;
        (void)uv;
        (void)uvStep;
        return false;
#endif
    }

    void Close() {
#ifdef _WIN32
        if (memDc_ && old_) SelectObject(memDc_, old_);
        if (bmp_) DeleteObject(bmp_);
        if (memDc_) DeleteDC(memDc_);
        if (screenDc_) ReleaseDC(nullptr, screenDc_);
        bmp_ = nullptr;
        memDc_ = screenDc_ = nullptr;
        old_ = nullptr;
        bits_ = nullptr;
#elif defined(__linux__) && !defined(__ANDROID__)
        if (dpy_ && closeDisplay_) closeDisplay_(dpy_);
        dpy_ = nullptr;
        if (lib_) dlclose(lib_);
        lib_ = nullptr;
#endif
    }

private:
    int sw_ = 0, sh_ = 0, dw_ = 0, dh_ = 0;
#ifdef _WIN32
    int sx_ = 0, sy_ = 0;
    HDC screenDc_ = nullptr, memDc_ = nullptr;
    HBITMAP bmp_ = nullptr;
    HGDIOBJ old_ = nullptr;
    uint8_t* bits_ = nullptr;
#elif defined(__linux__) && !defined(__ANDROID__)
    // the start of Xlib's XImage (its layout is fixed: X11R6)
    struct XImg {
        int width, height, xoffset, format;
        char* data;
        int byte_order, bitmap_unit, bitmap_bit_order, bitmap_pad, depth, bytes_per_line, bits_per_pixel;
        unsigned long red_mask, green_mask, blue_mask;
        char* obdata;
        void* (*create)(void*, void*, unsigned, int, int, char*, unsigned, unsigned, int, int);
        int (*destroy)(XImg*);
    };
    void* lib_ = nullptr;
    void* dpy_ = nullptr;
    unsigned long root_ = 0;
    void* (*openDisplay_)(const char*) = nullptr;
    int (*closeDisplay_)(void*) = nullptr;
    int (*defScreen_)(void*) = nullptr;
    unsigned long (*rootWindow_)(void*, int) = nullptr;
    int (*dispW_)(void*, int) = nullptr;
    int (*dispH_)(void*, int) = nullptr;
    XImg* (*getImage_)(void*, unsigned long, int, int, unsigned, unsigned, unsigned long, int) = nullptr;
#endif
};

}  // namespace lsv
