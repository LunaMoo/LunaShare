// LunaShare Camera: a Media Foundation media source that Windows 11's camera Frame Server loads for the virtual
// camera LunaShare registers with MFCreateVirtualCamera. It serves NV12 pictures that LunaShare writes into shared
// memory (the video received from a phone), letterboxed to whatever size the camera app picked; while nothing is
// coming in it shows a dark placeholder.
//
// Built as lunashare_vcam.dll (COM in-process server). LunaShare copies it to %ProgramData%\LunaShare and registers
// it once (needs admin), because the Frame Server service can't read DLLs inside user profiles.
//
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#include <initguid.h>
#include <windows.h>

#include <ks.h>
#include <ksmedia.h>
#include <strmif.h>
#include <ksproxy.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <new>
#include <string>

#include "vcam_shared.h"

// ---------------------------------------------------------------- declarations older SDKs/mingw lack
#if !defined(_MSC_VER)
MIDL_INTERFACE("C5BC37D6-75C7-46A1-A132-81B5F723C20F")
IMFMediaStream2 : public IMFMediaStream {
public:
    virtual HRESULT STDMETHODCALLTYPE SetStreamState(MF_STREAM_STATE value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStreamState(MF_STREAM_STATE * value) = 0;
};
__CRT_UUID_DECL(IMFMediaStream2, 0xc5bc37d6, 0x75c7, 0x46a1, 0xa1, 0x32, 0x81, 0xb5, 0xf7, 0x23, 0xc2, 0x0f)
DEFINE_GUID(MF_DEVICESTREAM_STREAM_CATEGORY, 0x2939e7b8, 0xa62e, 0x4579, 0xb6, 0x74, 0xd4, 0x07, 0x3d, 0xfa, 0xbb, 0xba);
DEFINE_GUID(MF_DEVICESTREAM_STREAM_ID, 0x11bd5120, 0xd124, 0x446b, 0x88, 0xe6, 0x17, 0x06, 0x02, 0x57, 0xff, 0xf9);
DEFINE_GUID(MF_DEVICESTREAM_FRAMESERVER_SHARED, 0x1cb378e9, 0xb279, 0x41d4, 0xaf, 0x97, 0x34, 0xa2, 0x43, 0xe6, 0x83, 0x20);
DEFINE_GUID(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, 0x17145fd1, 0x1b2b, 0x423c, 0x80, 0x01, 0x2b, 0x68, 0x33, 0xed, 0x35, 0x88);
#ifndef MFFrameSourceTypes_Color
#define MFFrameSourceTypes_Color 0x0001
#endif
__CRT_UUID_DECL(IKsControl, 0x28F54685, 0x06FD, 0x11D2, 0xB2, 0x7A, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96)
#endif

// {29D076F7-8632-4FB9-B2C5-9819DA3B835B}
static const GUID CLSID_LunaShareCamera = {0x29d076f7, 0x8632, 0x4fb9, {0xb2, 0xc5, 0x98, 0x19, 0xda, 0x3b, 0x83, 0x5b}};

// PINNAME_VIDEO_CAPTURE (ksmedia.h declares it as a struct GUID that isn't always linkable)
static const GUID kPinVideoCapture = {0xfb6c4281, 0x0353, 0x11d1, {0x90, 0x5f, 0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba}};

static HMODULE g_module = nullptr;
static std::atomic<long> g_objects{0}, g_locks{0};

namespace {

// ---------------------------------------------------------------- shared memory (created here, written by LunaShare)
class SharedFrames {
public:
    bool Open() {
        if (view_) return true;
        // the writer runs as the logged-in user: let authenticated users read/write, the system/service full access
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;BA)(A;;GRGW;;;AU)", SDDL_REVISION_1, &sd, nullptr))
            sd = nullptr;
        SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
        unsigned long long size = LSVC_MAPPING_SIZE;
        map_ = CreateFileMappingW(INVALID_HANDLE_VALUE, sd ? &sa : nullptr, PAGE_READWRITE, (DWORD)(size >> 32), (DWORD)size, LSVC_MAPPING);
        if (sd) LocalFree(sd);
        if (!map_) return false;
        view_ = (uint8_t*)MapViewOfFile(map_, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (!view_) {
            CloseHandle(map_);
            map_ = nullptr;
            return false;
        }
        auto* h = (LsvcHeader*)view_;
        if (h->magic != LSVC_MAGIC) {
            memset(h, 0, sizeof(*h));
            h->version = 1;
            h->magic = LSVC_MAGIC;
        }
        return true;
    }
    void Close() {
        if (view_) UnmapViewOfFile(view_);
        if (map_) CloseHandle(map_);
        view_ = nullptr;
        map_ = nullptr;
    }
    LsvcHeader* Header() { return (LsvcHeader*)view_; }
    const uint8_t* Data() { return view_ + LSVC_DATA_OFFSET; }

private:
    HANDLE map_ = nullptr;
    uint8_t* view_ = nullptr;
};

// letterbox the shared NV12 picture into an NV12 output of size ow x oh (nearest neighbour; cheap and good enough
// since the writer already sends roughly the requested size)
void Render(SharedFrames& sh, uint8_t* out, int ow, int oh) {
    uint8_t* oy = out;
    uint8_t* ouv = out + (size_t)ow * oh;
    LsvcHeader* h = sh.Header();
    bool fresh = h && h->width && h->height && GetTickCount64() - (uint64_t)h->writtenMs < 2000;
    if (h) {
        h->wantW = (uint32_t)ow;
        h->wantH = (uint32_t)oh;
    }
    if (!fresh) {
        // placeholder: dark blue-grey
        memset(oy, 40, (size_t)ow * oh);
        for (size_t i = 0; i < (size_t)ow * (oh / 2); i += 2) ouv[i] = 140, ouv[i + 1] = 118;
        return;
    }
    for (int attempt = 0; attempt < 3; attempt++) {
        long s0 = h->seq;
        if (s0 & 1) {
            Sleep(1);
            continue;
        }
        int sw = (int)h->width, shh = (int)h->height;
        if (sw <= 0 || shh <= 0 || sw > LSVC_MAX_W || shh > LSVC_MAX_H) return;
        const uint8_t* sy = sh.Data();
        const uint8_t* suv = sy + (size_t)sw * shh;
        double f = std::min((double)ow / sw, (double)oh / shh);
        int dw = std::max(2, (int)(sw * f) & ~1), dh = std::max(2, (int)(shh * f) & ~1);
        int x0 = ((ow - dw) / 2) & ~1, y0 = ((oh - dh) / 2) & ~1;
        memset(oy, 16, (size_t)ow * oh);
        for (size_t i = 0; i < (size_t)ow * (oh / 2); i++) ouv[i] = 128;
        for (int y = 0; y < dh; y++) {
            int syy = (int)((int64_t)y * shh / dh);
            const uint8_t* srow = sy + (size_t)syy * sw;
            uint8_t* drow = oy + (size_t)(y0 + y) * ow + x0;
            for (int x = 0; x < dw; x++) drow[x] = srow[(int64_t)x * sw / dw];
        }
        for (int y = 0; y < dh / 2; y++) {
            int syy = (int)((int64_t)y * (shh / 2) / (dh / 2));
            const uint8_t* srow = suv + (size_t)syy * sw;
            uint8_t* drow = ouv + (size_t)(y0 / 2 + y) * ow + x0;
            for (int x = 0; x < dw / 2; x++) {
                int sx = (int)((int64_t)x * (sw / 2) / (dw / 2)) * 2;
                drow[x * 2] = srow[sx];
                drow[x * 2 + 1] = srow[sx + 1];
            }
        }
        if (h->seq == s0) return;  // not overwritten while we copied
    }
}

// ---------------------------------------------------------------- media types we offer
struct Mode {
    int w, h, fps;
};
const Mode kModes[] = {{1920, 1080, 30}, {1280, 720, 30}, {3840, 2160, 30}, {2560, 1440, 30},
                       {1920, 1080, 60}, {1280, 720, 60}, {640, 360, 30}};

HRESULT MakeType(const Mode& m, IMFMediaType** out) {
    IMFMediaType* t = nullptr;
    HRESULT hr = MFCreateMediaType(&t);
    if (FAILED(hr)) return hr;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(t, MF_MT_FRAME_SIZE, (UINT32)m.w, (UINT32)m.h);
    MFSetAttributeRatio(t, MF_MT_FRAME_RATE, (UINT32)m.fps, 1);
    MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    t->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    t->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)m.w);
    t->SetUINT32(MF_MT_SAMPLE_SIZE, (UINT32)(m.w * m.h * 3 / 2));
    t->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)std::min<int64_t>(0xffffffffLL, (int64_t)m.w * m.h * 12 * m.fps));
    *out = t;
    return S_OK;
}

class Source;

// ---------------------------------------------------------------- stream
class Stream : public IMFMediaStream2 {
public:
    Stream(Source* src, IMFStreamDescriptor* sd) : source_(src), sd_(sd) {
        sd_->AddRef();
        MFCreateEventQueue(&queue_);
        g_objects++;
    }
    ~Stream() {
        if (queue_) queue_->Release();
        if (sd_) sd_->Release();
        frames_.Close();
        g_objects--;
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IMFMediaEventGenerator || riid == IID_IMFMediaStream || riid == __uuidof(IMFMediaStream2))
            *ppv = static_cast<IMFMediaStream2*>(this);
        else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override {
        long r = --ref_;
        if (!r) delete this;
        return r;
    }

    // IMFMediaEventGenerator
    STDMETHODIMP GetEvent(DWORD f, IMFMediaEvent** e) override {
        IMFMediaEventQueue* q = Queue();
        if (!q) return MF_E_SHUTDOWN;
        HRESULT hr = q->GetEvent(f, e);
        q->Release();
        return hr;
    }
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* st) override {
        IMFMediaEventQueue* q = Queue();
        if (!q) return MF_E_SHUTDOWN;
        HRESULT hr = q->BeginGetEvent(cb, st);
        q->Release();
        return hr;
    }
    STDMETHODIMP EndGetEvent(IMFAsyncResult* r, IMFMediaEvent** e) override {
        IMFMediaEventQueue* q = Queue();
        if (!q) return MF_E_SHUTDOWN;
        HRESULT hr = q->EndGetEvent(r, e);
        q->Release();
        return hr;
    }
    STDMETHODIMP QueueEvent(MediaEventType t, REFGUID g, HRESULT hs, const PROPVARIANT* v) override {
        IMFMediaEventQueue* q = Queue();
        if (!q) return MF_E_SHUTDOWN;
        HRESULT hr = q->QueueEventParamVar(t, g, hs, v);
        q->Release();
        return hr;
    }

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** s) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** sd) override {
        if (!sd) return E_POINTER;
        std::lock_guard<std::mutex> l(m_);
        if (shutdown_) return MF_E_SHUTDOWN;
        *sd = sd_;
        sd_->AddRef();
        return S_OK;
    }
    STDMETHODIMP RequestSample(IUnknown* token) override {
        std::lock_guard<std::mutex> l(m_);
        if (shutdown_) return MF_E_SHUTDOWN;
        if (state_ != MF_STREAM_STATE_RUNNING) return MF_E_INVALIDREQUEST;
        int w = 1920, h = 1080, fps = 30;
        CurrentMode(&w, &h, &fps);
        DWORD size = (DWORD)(w * h * 3 / 2);
        IMFSample* s = nullptr;
        IMFMediaBuffer* b = nullptr;
        HRESULT hr = MFCreateSample(&s);
        if (SUCCEEDED(hr)) hr = MFCreateMemoryBuffer(size, &b);
        BYTE* p = nullptr;
        if (SUCCEEDED(hr)) hr = b->Lock(&p, nullptr, nullptr);
        if (SUCCEEDED(hr)) {
            if (frames_.Open()) Render(frames_, p, w, h);
            else memset(p, 40, size);
            b->Unlock();
            b->SetCurrentLength(size);
            s->AddBuffer(b);
            s->SetSampleTime(MFGetSystemTime());
            s->SetSampleDuration(10000000LL / fps);
            s->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
            if (token) s->SetUnknown(MFSampleExtension_Token, token);
            hr = queue_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, s);
        }
        if (b) b->Release();
        if (s) s->Release();
        return hr;
    }

    // IMFMediaStream2
    STDMETHODIMP SetStreamState(MF_STREAM_STATE v) override {
        std::lock_guard<std::mutex> l(m_);
        if (shutdown_) return MF_E_SHUTDOWN;
        if (v == state_) return S_OK;
        if (v == MF_STREAM_STATE_PAUSED) return MF_E_INVALID_STATE_TRANSITION;
        state_ = v;
        return queue_->QueueEventParamVar(v == MF_STREAM_STATE_RUNNING ? MEStreamStarted : MEStreamStopped, GUID_NULL, S_OK, nullptr);
    }
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* v) override {
        if (!v) return E_POINTER;
        std::lock_guard<std::mutex> l(m_);
        *v = state_;
        return S_OK;
    }

    HRESULT Start(const PROPVARIANT* pos) {
        std::lock_guard<std::mutex> l(m_);
        if (shutdown_) return MF_E_SHUTDOWN;
        state_ = MF_STREAM_STATE_RUNNING;
        return queue_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, pos);
    }
    HRESULT Stop() {
        std::lock_guard<std::mutex> l(m_);
        if (shutdown_) return MF_E_SHUTDOWN;
        state_ = MF_STREAM_STATE_STOPPED;
        return queue_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
    }
    void Shutdown() {
        std::lock_guard<std::mutex> l(m_);
        if (shutdown_) return;
        shutdown_ = true;
        if (queue_) queue_->Shutdown();
        frames_.Close();
    }

private:
    IMFMediaEventQueue* Queue() {
        std::lock_guard<std::mutex> l(m_);
        if (shutdown_ || !queue_) return nullptr;
        queue_->AddRef();
        return queue_;
    }
    void CurrentMode(int* w, int* h, int* fps) {
        IMFMediaTypeHandler* th = nullptr;
        IMFMediaType* t = nullptr;
        if (SUCCEEDED(sd_->GetMediaTypeHandler(&th)) && SUCCEEDED(th->GetCurrentMediaType(&t))) {
            UINT32 a = 0, b = 0;
            if (SUCCEEDED(MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &a, &b)) && a && b) *w = (int)a, *h = (int)b;
            if (SUCCEEDED(MFGetAttributeRatio(t, MF_MT_FRAME_RATE, &a, &b)) && a && b) *fps = std::max(1, (int)(a / b));
        }
        if (t) t->Release();
        if (th) th->Release();
    }

    std::atomic<long> ref_{1};
    std::mutex m_;
    Source* source_;
    IMFStreamDescriptor* sd_ = nullptr;
    IMFMediaEventQueue* queue_ = nullptr;
    MF_STREAM_STATE state_ = MF_STREAM_STATE_STOPPED;
    bool shutdown_ = false;
    SharedFrames frames_;
};

// ---------------------------------------------------------------- source
class Source : public IMFMediaSourceEx, public IMFGetService, public IKsControl {
public:
    Source() { g_objects++; }
    ~Source() {
        if (stream_) stream_->Release();
        if (pd_) pd_->Release();
        if (attrs_) attrs_->Release();
        if (queue_) queue_->Release();
        g_objects--;
    }

    HRESULT Init() {
        HRESULT hr = MFCreateEventQueue(&queue_);
        if (SUCCEEDED(hr)) hr = MFCreateAttributes(&attrs_, 4);
        const int n = (int)(sizeof(kModes) / sizeof(kModes[0]));
        IMFMediaType* types[n] = {};
        for (int i = 0; SUCCEEDED(hr) && i < n; i++) hr = MakeType(kModes[i], &types[i]);
        IMFStreamDescriptor* sd = nullptr;
        if (SUCCEEDED(hr)) hr = MFCreateStreamDescriptor(0, n, types, &sd);
        if (SUCCEEDED(hr)) {
            sd->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, kPinVideoCapture);
            sd->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
            sd->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
            sd->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);
            IMFMediaTypeHandler* th = nullptr;
            if (SUCCEEDED(sd->GetMediaTypeHandler(&th))) {
                th->SetCurrentMediaType(types[0]);
                th->Release();
            }
            hr = MFCreatePresentationDescriptor(1, &sd, &pd_);
        }
        if (SUCCEEDED(hr)) hr = pd_->SelectStream(0);
        if (SUCCEEDED(hr)) stream_ = new (std::nothrow) Stream(this, sd);
        if (SUCCEEDED(hr) && !stream_) hr = E_OUTOFMEMORY;
        if (sd) sd->Release();
        for (auto* t : types)
            if (t) t->Release();
        return hr;
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IMFMediaEventGenerator || riid == IID_IMFMediaSource || riid == IID_IMFMediaSourceEx)
            *ppv = static_cast<IMFMediaSourceEx*>(this);
        else if (riid == IID_IMFGetService)
            *ppv = static_cast<IMFGetService*>(this);
        else if (riid == __uuidof(IKsControl))
            *ppv = static_cast<IKsControl*>(this);
        else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override {
        long r = --ref_;
        if (!r) delete this;
        return r;
    }

    // IMFMediaEventGenerator
    STDMETHODIMP GetEvent(DWORD f, IMFMediaEvent** e) override {
        IMFMediaEventQueue* q = Queue();
        if (!q) return MF_E_SHUTDOWN;
        HRESULT hr = q->GetEvent(f, e);
        q->Release();
        return hr;
    }
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* st) override {
        IMFMediaEventQueue* q = Queue();
        if (!q) return MF_E_SHUTDOWN;
        HRESULT hr = q->BeginGetEvent(cb, st);
        q->Release();
        return hr;
    }
    STDMETHODIMP EndGetEvent(IMFAsyncResult* r, IMFMediaEvent** e) override {
        IMFMediaEventQueue* q = Queue();
        if (!q) return MF_E_SHUTDOWN;
        HRESULT hr = q->EndGetEvent(r, e);
        q->Release();
        return hr;
    }
    STDMETHODIMP QueueEvent(MediaEventType t, REFGUID g, HRESULT hs, const PROPVARIANT* v) override {
        IMFMediaEventQueue* q = Queue();
        if (!q) return MF_E_SHUTDOWN;
        HRESULT hr = q->QueueEventParamVar(t, g, hs, v);
        q->Release();
        return hr;
    }

    // IMFMediaSource
    STDMETHODIMP GetCharacteristics(DWORD* c) override {
        if (!c) return E_POINTER;
        if (shutdown_) return MF_E_SHUTDOWN;
        *c = MFMEDIASOURCE_IS_LIVE;
        return S_OK;
    }
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** pd) override {
        if (!pd) return E_POINTER;
        std::lock_guard<std::mutex> l(m_);
        if (shutdown_) return MF_E_SHUTDOWN;
        return pd_->Clone(pd);
    }
    STDMETHODIMP Start(IMFPresentationDescriptor* pd, const GUID* fmt, const PROPVARIANT* pos) override {
        if (!pd || !pos) return E_INVALIDARG;
        if (fmt && *fmt != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;
        std::lock_guard<std::mutex> l(m_);
        if (shutdown_) return MF_E_SHUTDOWN;
        // take over the media type the camera app chose
        BOOL selected = FALSE;
        IMFStreamDescriptor* sd = nullptr;
        HRESULT hr = pd->GetStreamDescriptorByIndex(0, &selected, &sd);
        if (FAILED(hr)) return hr;
        if (selected) {
            IMFMediaTypeHandler *in = nullptr, *mine = nullptr;
            IMFMediaType* t = nullptr;
            IMFStreamDescriptor* my = nullptr;
            if (SUCCEEDED(sd->GetMediaTypeHandler(&in)) && SUCCEEDED(in->GetCurrentMediaType(&t)) && SUCCEEDED(stream_->GetStreamDescriptor(&my)) &&
                SUCCEEDED(my->GetMediaTypeHandler(&mine)))
                mine->SetCurrentMediaType(t);
            if (mine) mine->Release();
            if (my) my->Release();
            if (t) t->Release();
            if (in) in->Release();
            hr = queue_->QueueEventParamUnk(started_ ? MEUpdatedStream : MENewStream, GUID_NULL, S_OK, static_cast<IMFMediaStream2*>(stream_));
            if (SUCCEEDED(hr)) hr = stream_->Start(pos);
        }
        sd->Release();
        if (FAILED(hr)) return hr;
        started_ = true;
        return queue_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, pos);
    }
    STDMETHODIMP Stop() override {
        std::lock_guard<std::mutex> l(m_);
        if (shutdown_) return MF_E_SHUTDOWN;
        stream_->Stop();
        return queue_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
    }
    STDMETHODIMP Pause() override { return MF_E_INVALID_STATE_TRANSITION; }
    STDMETHODIMP Shutdown() override {
        std::lock_guard<std::mutex> l(m_);
        if (shutdown_) return MF_E_SHUTDOWN;
        shutdown_ = true;
        if (stream_) stream_->Shutdown();
        if (queue_) queue_->Shutdown();
        return S_OK;
    }

    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** a) override {
        if (!a) return E_POINTER;
        if (shutdown_) return MF_E_SHUTDOWN;
        *a = attrs_;
        attrs_->AddRef();
        return S_OK;
    }
    STDMETHODIMP GetStreamAttributes(DWORD id, IMFAttributes** a) override {
        if (!a) return E_POINTER;
        if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
        if (shutdown_) return MF_E_SHUTDOWN;
        IMFStreamDescriptor* sd = nullptr;
        HRESULT hr = stream_->GetStreamDescriptor(&sd);
        if (SUCCEEDED(hr)) {
            hr = sd->QueryInterface(IID_IMFAttributes, (void**)a);
            sd->Release();
        }
        return hr;
    }
    STDMETHODIMP SetD3DManager(IUnknown*) override { return S_OK; }  // we hand out system-memory samples

    // IMFGetService
    STDMETHODIMP GetService(REFGUID, REFIID, LPVOID*) override { return MF_E_UNSUPPORTED_SERVICE; }

    // IKsControl: no camera properties (zoom etc. are controlled from LunaShare)
    STDMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }
    STDMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }
    STDMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }

    HRESULT GetStreamSourceRef(IMFMediaSource** s) {
        *s = static_cast<IMFMediaSourceEx*>(this);
        AddRef();
        return S_OK;
    }

private:
    IMFMediaEventQueue* Queue() {
        std::lock_guard<std::mutex> l(m_);
        if (shutdown_ || !queue_) return nullptr;
        queue_->AddRef();
        return queue_;
    }

    std::atomic<long> ref_{1};
    std::mutex m_;
    IMFMediaEventQueue* queue_ = nullptr;
    IMFAttributes* attrs_ = nullptr;
    IMFPresentationDescriptor* pd_ = nullptr;
    Stream* stream_ = nullptr;
    bool started_ = false, shutdown_ = false;
};

STDMETHODIMP Stream::GetMediaSource(IMFMediaSource** s) {
    if (!s) return E_POINTER;
    std::lock_guard<std::mutex> l(m_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return source_->GetStreamSourceRef(s);
}

// ---------------------------------------------------------------- activation object (what the Frame Server creates)
class Activate : public IMFActivate {
public:
    Activate() {
        MFCreateAttributes(&a_, 4);
        g_objects++;
    }
    ~Activate() {
        if (src_) src_->Release();
        if (a_) a_->Release();
        g_objects--;
    }
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IMFAttributes || riid == IID_IMFActivate) *ppv = static_cast<IMFActivate*>(this);
        else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override {
        long r = --ref_;
        if (!r) delete this;
        return r;
    }

    STDMETHODIMP ActivateObject(REFIID riid, void** ppv) override {
        std::lock_guard<std::mutex> l(m_);
        if (!src_) {
            auto* s = new (std::nothrow) Source();
            if (!s) return E_OUTOFMEMORY;
            HRESULT hr = s->Init();
            if (FAILED(hr)) {
                s->Release();
                return hr;
            }
            src_ = s;
        }
        return src_->QueryInterface(riid, ppv);
    }
    STDMETHODIMP ShutdownObject() override {
        std::lock_guard<std::mutex> l(m_);
        if (src_) {
            src_->Shutdown();
            src_->Release();
            src_ = nullptr;
        }
        return S_OK;
    }
    STDMETHODIMP DetachObject() override {
        std::lock_guard<std::mutex> l(m_);
        if (src_) src_->Release();
        src_ = nullptr;
        return S_OK;
    }

    // IMFAttributes: forwarded to a plain attribute store
#define FWD(name, params, args) STDMETHODIMP name params override { return a_->name args; }
    FWD(GetItem, (REFGUID k, PROPVARIANT* v), (k, v))
    FWD(GetItemType, (REFGUID k, MF_ATTRIBUTE_TYPE* t), (k, t))
    FWD(CompareItem, (REFGUID k, REFPROPVARIANT v, BOOL* r), (k, v, r))
    FWD(Compare, (IMFAttributes* t, MF_ATTRIBUTES_MATCH_TYPE m, BOOL* r), (t, m, r))
    FWD(GetUINT32, (REFGUID k, UINT32* v), (k, v))
    FWD(GetUINT64, (REFGUID k, UINT64* v), (k, v))
    FWD(GetDouble, (REFGUID k, double* v), (k, v))
    FWD(GetGUID, (REFGUID k, GUID* v), (k, v))
    FWD(GetStringLength, (REFGUID k, UINT32* v), (k, v))
    FWD(GetString, (REFGUID k, LPWSTR v, UINT32 n, UINT32* l), (k, v, n, l))
    FWD(GetAllocatedString, (REFGUID k, LPWSTR* v, UINT32* l), (k, v, l))
    FWD(GetBlobSize, (REFGUID k, UINT32* v), (k, v))
    FWD(GetBlob, (REFGUID k, UINT8* b, UINT32 n, UINT32* s), (k, b, n, s))
    FWD(GetAllocatedBlob, (REFGUID k, UINT8** b, UINT32* s), (k, b, s))
    FWD(GetUnknown, (REFGUID k, REFIID r, LPVOID* p), (k, r, p))
    FWD(SetItem, (REFGUID k, REFPROPVARIANT v), (k, v))
    FWD(DeleteItem, (REFGUID k), (k))
    FWD(DeleteAllItems, (), ())
    FWD(SetUINT32, (REFGUID k, UINT32 v), (k, v))
    FWD(SetUINT64, (REFGUID k, UINT64 v), (k, v))
    FWD(SetDouble, (REFGUID k, double v), (k, v))
    FWD(SetGUID, (REFGUID k, REFGUID v), (k, v))
    FWD(SetString, (REFGUID k, LPCWSTR v), (k, v))
    FWD(SetBlob, (REFGUID k, const UINT8* b, UINT32 s), (k, b, s))
    FWD(SetUnknown, (REFGUID k, IUnknown* u), (k, u))
    FWD(LockStore, (), ())
    FWD(UnlockStore, (), ())
    FWD(GetCount, (UINT32 * c), (c))
    FWD(GetItemByIndex, (UINT32 i, GUID* k, PROPVARIANT* v), (i, k, v))
    FWD(CopyAllItems, (IMFAttributes * d), (d))
#undef FWD

private:
    std::atomic<long> ref_{1};
    std::mutex m_;
    IMFAttributes* a_ = nullptr;
    Source* src_ = nullptr;
};

class Factory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* a = new (std::nothrow) Activate();
        if (!a) return E_OUTOFMEMORY;
        HRESULT hr = a->QueryInterface(riid, ppv);
        a->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) g_locks++;
        else g_locks--;
        return S_OK;
    }
};
Factory g_factory;

LONG SetKey(HKEY root, const std::wstring& path, const wchar_t* name, const std::wstring& value) {
    HKEY k = nullptr;
    LONG r = RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr);
    if (r != ERROR_SUCCESS) return r;
    r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r;
}

}  // namespace

// ---------------------------------------------------------------- COM server exports (see vcam.def)
extern "C" BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = h;
        DisableThreadLibraryCalls(h);
    }
    return TRUE;
}

STDAPI DllCanUnloadNow() { return (g_objects == 0 && g_locks == 0) ? S_OK : S_FALSE; }

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* ppv) {
    if (clsid != CLSID_LunaShareCamera) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(g_module, path, MAX_PATH)) return HRESULT_FROM_WIN32(GetLastError());
    std::wstring key = std::wstring(L"Software\\Classes\\CLSID\\") + LSVC_CLSID_STR;
    LONG r = SetKey(HKEY_LOCAL_MACHINE, key, nullptr, LSVC_NAME);
    if (r == ERROR_SUCCESS) r = SetKey(HKEY_LOCAL_MACHINE, key + L"\\InprocServer32", nullptr, path);
    if (r == ERROR_SUCCESS) r = SetKey(HKEY_LOCAL_MACHINE, key + L"\\InprocServer32", L"ThreadingModel", L"Both");
    return HRESULT_FROM_WIN32(r);
}

STDAPI DllUnregisterServer() {
    std::wstring key = std::wstring(L"Software\\Classes\\CLSID\\") + LSVC_CLSID_STR;
    LONG r = RegDeleteTreeW(HKEY_LOCAL_MACHINE, key.c_str());
    return (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND) ? S_OK : HRESULT_FROM_WIN32(r);
}
