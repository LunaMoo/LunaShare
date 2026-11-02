// Emoji backend for Windows: Segoe UI Emoji through DirectWrite + Direct2D (colour fonts need Windows 8.1+),
// drawn into a WIC bitmap. DirectWrite shapes the text, so ZWJ sequences, skin tones and keycaps work as in other apps.
// SPDX-License-Identifier: GPL-3.0-only
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>

#include "emoji_backend.h"

namespace lse {
namespace {

template <class T>
void Release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

ID2D1Factory* g_d2d = nullptr;
IDWriteFactory* g_dw = nullptr;
IWICImagingFactory* g_wic = nullptr;
// one bitmap + render target, recreated when the size changes
IWICBitmap* g_bmp = nullptr;
ID2D1RenderTarget* g_rt = nullptr;
ID2D1DeviceContext* g_dc = nullptr;
ID2D1SolidColorBrush* g_brush = nullptr;
int g_size = 0;

std::wstring Wide(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? (size_t)n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

bool Target(int size) {
    if (g_size == size && g_dc) return true;
    Release(g_brush);
    Release(g_dc);
    Release(g_rt);
    Release(g_bmp);
    g_size = 0;
    if (FAILED(g_wic->CreateBitmap((UINT)size, (UINT)size, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &g_bmp))) return false;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(g_d2d->CreateWicBitmapRenderTarget(g_bmp, &props, &g_rt))) return false;
    if (FAILED(g_rt->QueryInterface(__uuidof(ID2D1DeviceContext), (void**)&g_dc))) return false;  // colour fonts
    g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (FAILED(g_rt->CreateSolidColorBrush(D2D1::ColorF(0.85f, 0.85f, 0.85f), &g_brush))) return false;
    g_size = size;
    return true;
}

}  // namespace

bool BackendInit(const LseHost*, std::string* why) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // fine if COM is already set up differently
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, (void**)&g_d2d))) {
        *why = "Direct2D unavailable";
        return false;
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dw))) {
        *why = "DirectWrite unavailable";
        return false;
    }
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&g_wic))) {
        *why = "WIC unavailable";
        return false;
    }
    if (!Target(64)) {
        *why = "no colour font support (needs Windows 8.1 or newer)";
        return false;
    }
    // is there an emoji font at all?
    IDWriteFontCollection* fonts = nullptr;
    UINT32 idx = 0;
    BOOL exists = FALSE;
    if (SUCCEEDED(g_dw->GetSystemFontCollection(&fonts, FALSE)) && fonts) fonts->FindFamilyName(L"Segoe UI Emoji", &idx, &exists);
    Release(fonts);
    if (!exists) {
        *why = "Segoe UI Emoji not installed";
        return false;
    }
    return true;
}

bool BackendRender(const char* utf8, int size, std::vector<uint8_t>& rgba) {
    if (!Target(size)) return false;
    std::wstring text = Wide(utf8);
    if (text.empty()) return false;
    // lay it out at a reference size, measure the ink, then scale it into the square
    IDWriteTextFormat* fmt = nullptr;
    if (FAILED(g_dw->CreateTextFormat(L"Segoe UI Emoji", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, 100.0f, L"en-us", &fmt)))
        return false;
    IDWriteTextLayout* layout = nullptr;
    HRESULT hr = g_dw->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt, 400.0f, 400.0f, &layout);
    Release(fmt);
    if (FAILED(hr)) return false;
    DWRITE_TEXT_METRICS tm{};
    DWRITE_OVERHANG_METRICS om{};
    layout->GetMetrics(&tm);
    layout->GetOverhangMetrics(&om);
    // more than one cluster means the font had no picture for the whole sequence: show it anyway (like Windows does)
    float x0 = -om.left, y0 = -om.top, x1 = 400.0f + om.right, y1 = 400.0f + om.bottom;
    float iw = x1 - x0, ih = y1 - y0;
    if (iw <= 1 || ih <= 1 || tm.width <= 0) {
        Release(layout);
        return false;
    }
    float k = size * 0.96f / std::max(iw, ih);
    float tx = size * 0.5f - (x0 + iw * 0.5f) * k, ty = size * 0.5f - (y0 + ih * 0.5f) * k;
    g_dc->BeginDraw();
    g_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    g_dc->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(tx, ty));
    g_dc->DrawTextLayout(D2D1::Point2F(0, 0), layout, g_brush, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    hr = g_dc->EndDraw();
    Release(layout);
    if (FAILED(hr)) {
        g_size = 0;  // device lost etc.: recreate next time
        return false;
    }
    WICRect r{0, 0, size, size};
    IWICBitmapLock* lock = nullptr;
    if (FAILED(g_bmp->Lock(&r, WICBitmapLockRead, &lock))) return false;
    UINT stride = 0, bytes = 0;
    BYTE* px = nullptr;
    lock->GetStride(&stride);
    lock->GetDataPointer(&bytes, &px);
    bool any = false;
    for (int y = 0; y < size && px; y++) {
        const BYTE* row = px + (size_t)y * stride;
        for (int x = 0; x < size; x++) {
            const BYTE* p = row + x * 4;
            uint8_t* o = &rgba[((size_t)y * size + x) * 4];
            int a = p[3];
            if (!a) continue;
            any = true;
            o[0] = (uint8_t)std::min(255, p[2] * 255 / a);
            o[1] = (uint8_t)std::min(255, p[1] * 255 / a);
            o[2] = (uint8_t)std::min(255, p[0] * 255 / a);
            o[3] = (uint8_t)a;
        }
    }
    Release(lock);
    return any;
}

}  // namespace lse
