// Video backend for Windows: Media Foundation camera capture + H.264 encoder/decoder MFTs.
//
// Encoder: a hardware H.264 MFT (GPU) when one works with system-memory frames, otherwise Microsoft's software
// encoder. Decoder: Microsoft's H.264 decoder. Camera controls go through IAMCameraControl / IAMVideoProcAmp.
//
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <initguid.h>
#include <windows.h>

#include <codecapi.h>
#include <d3d11.h>
#include <devpropdef.h>
#include <shellapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <strmif.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "vcam_shared.h"
#include "scale.h"
#include "screen_grab.h"
#include "video_backend.h"

// Windows 11 virtual camera API (mfvirtualcamera.h, SDK 10.0.22000+). Loaded at run time from mfsensorgroup.dll so the
// module still loads on older Windows; declared here when the SDK headers don't have it.
#if defined(_MSC_VER) && __has_include(<mfvirtualcamera.h>)
#include <mfvirtualcamera.h>
#else
typedef enum __MFVirtualCameraType { MFVirtualCameraType_SoftwareCameraSource = 0 } MFVirtualCameraType;
typedef enum __MFVirtualCameraLifetime { MFVirtualCameraLifetime_Session = 0, MFVirtualCameraLifetime_System = 1 } MFVirtualCameraLifetime;
typedef enum __MFVirtualCameraAccess { MFVirtualCameraAccess_CurrentUser = 0, MFVirtualCameraAccess_AllUsers = 1 } MFVirtualCameraAccess;
struct IMFCameraSyncObject;
MIDL_INTERFACE("1C08A864-EF6C-4C75-AF59-5F2D68DA9563")
IMFVirtualCamera : public IMFAttributes {
public:
    virtual HRESULT STDMETHODCALLTYPE AddDeviceSourceInfo(LPCWSTR info) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddProperty(const DEVPROPKEY* key, DEVPROPTYPE type, const BYTE* data, ULONG size) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddRegistryEntry(LPCWSTR name, LPCWSTR subkey, DWORD type, const BYTE* data, ULONG size) = 0;
    virtual HRESULT STDMETHODCALLTYPE Start(IMFAsyncCallback * callback) = 0;
    virtual HRESULT STDMETHODCALLTYPE Stop() = 0;
    virtual HRESULT STDMETHODCALLTYPE Remove() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMediaSource(IMFMediaSource * *source) = 0;
    virtual HRESULT STDMETHODCALLTYPE SendCameraProperty(REFGUID set, ULONG id, ULONG flags, void* payload, ULONG payloadLen, void* data,
                                                         ULONG dataLen, ULONG* written) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSyncEvent(REFGUID set, ULONG id, ULONG flags, HANDLE ev, IMFCameraSyncObject** obj) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSyncSemaphore(REFGUID set, ULONG id, ULONG flags, HANDLE sem, LONG adjust,
                                                          IMFCameraSyncObject** obj) = 0;
    virtual HRESULT STDMETHODCALLTYPE Shutdown() = 0;
};
#endif

#if defined(__MINGW32__) && !defined(_MSC_VER)
__CRT_UUID_DECL(ICodecAPI, 0x901db4c7, 0x31ce, 0x41a2, 0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda)
#endif

namespace lsv {
namespace {

template <class T>
class Com {
public:
    Com() = default;
    Com(const Com& o) : p_(o.p_) { if (p_) p_->AddRef(); }
    ~Com() { Reset(); }
    Com& operator=(const Com& o) {
        if (o.p_) o.p_->AddRef();
        Reset();
        p_ = o.p_;
        return *this;
    }
    void Reset() {
        if (p_) p_->Release();
        p_ = nullptr;
    }
    T** operator&() {
        Reset();
        return &p_;
    }
    T* operator->() const { return p_; }
    T* get() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    template <class U>
    Com<U> As() const {
        Com<U> r;
        if (p_) p_->QueryInterface(__uuidof(U), (void**)&r);
        return r;
    }

private:
    T* p_ = nullptr;
};

std::string Narrow(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}
std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
std::string Hr(HRESULT hr) {
    char b[32];
    snprintf(b, sizeof(b), "0x%08lx", (unsigned long)hr);
    return b;
}

struct ComInit {
    bool ok;
    ComInit() { ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
    ~ComInit() { if (ok) CoUninitialize(); }
};

void SetCodecU32(ICodecAPI* api, const GUID& key, UINT32 v) {
    if (!api) return;
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_UI4;
    var.ulVal = v;
    api->SetValue(&key, &var);
}
void SetCodecBool(ICodecAPI* api, const GUID& key, bool v) {
    if (!api) return;
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_BOOL;
    var.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
    api->SetValue(&key, &var);
}

Com<IMFSample> MakeSample(DWORD size) {
    Com<IMFSample> s;
    Com<IMFMediaBuffer> b;
    if (FAILED(MFCreateSample(&s)) || FAILED(MFCreateMemoryBuffer(size, &b))) return {};
    s->AddBuffer(b.get());
    return s;
}

// ======================================================================== H.264 encoder (sync or async MFT)
class Encoder {
public:
    std::string name;

    bool Open(int w, int h, int fps, int bitrate, bool hardware, std::string* err) {
        w_ = w;
        h_ = h;
        fps_ = fps;
        MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_NV12}, out{MFMediaType_Video, MFVideoFormat_H264};
        IMFActivate** acts = nullptr;
        UINT32 n = 0;
        UINT32 flags = (hardware ? MFT_ENUM_FLAG_HARDWARE : MFT_ENUM_FLAG_SYNCMFT) | MFT_ENUM_FLAG_SORTANDFILTER;
        if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &in, &out, &acts, &n)) || n == 0) {
            if (err) *err = hardware ? "no hardware H.264 encoder" : "no H.264 encoder";
            return false;
        }
        HRESULT hr = acts[0]->ActivateObject(IID_PPV_ARGS(&mft_));
        WCHAR* fname = nullptr;
        UINT32 flen = 0;
        if (SUCCEEDED(acts[0]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &fname, &flen))) {
            name = Narrow(fname);
            CoTaskMemFree(fname);
        }
        for (UINT32 i = 0; i < n; i++) acts[i]->Release();
        CoTaskMemFree(acts);
        if (FAILED(hr)) {
            if (err) *err = "cannot create encoder " + Hr(hr);
            return false;
        }
        Com<IMFAttributes> attrs;
        if (SUCCEEDED(mft_->GetAttributes(&attrs)) && attrs) {
            UINT32 async = 0;
            if (SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &async)) && async) {
                attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                async_ = true;
            }
            attrs->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);  // same GUID as MF_LOW_LATENCY
        }
        DWORD ni = 0, no = 0;
        if (SUCCEEDED(mft_->GetStreamCount(&ni, &no)) && ni == 1 && no == 1) {
            if (mft_->GetStreamIDs(1, &inId_, 1, &outId_) == E_NOTIMPL) inId_ = outId_ = 0;
        }
        api_ = mft_.As<ICodecAPI>();
        SetCodecBool(api_.get(), CODECAPI_AVLowLatencyMode, true);
        SetCodecU32(api_.get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        SetCodecU32(api_.get(), CODECAPI_AVEncCommonMeanBitRate, (UINT32)bitrate);
        SetCodecU32(api_.get(), CODECAPI_AVEncMPVGOPSize, (UINT32)(fps * 4));
        SetCodecU32(api_.get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);

        Com<IMFMediaType> ot;
        MFCreateMediaType(&ot);
        ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        ot->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        ot->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)bitrate);
        MFSetAttributeSize(ot.get(), MF_MT_FRAME_SIZE, (UINT32)w, (UINT32)h);
        MFSetAttributeRatio(ot.get(), MF_MT_FRAME_RATE, (UINT32)fps, 1);
        MFSetAttributeRatio(ot.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        ot->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        ot->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
        if (FAILED(hr = mft_->SetOutputType(outId_, ot.get(), 0))) {
            // some encoders only do Main profile
            ot->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
            if (FAILED(hr = mft_->SetOutputType(outId_, ot.get(), 0))) {
                if (err) *err = "encoder output type " + Hr(hr);
                return false;
            }
        }
        Com<IMFMediaType> it;
        MFCreateMediaType(&it);
        it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        it->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(it.get(), MF_MT_FRAME_SIZE, (UINT32)w, (UINT32)h);
        MFSetAttributeRatio(it.get(), MF_MT_FRAME_RATE, (UINT32)fps, 1);
        MFSetAttributeRatio(it.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        it->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (FAILED(hr = mft_->SetInputType(inId_, it.get(), 0))) {
            if (err) *err = "encoder input type " + Hr(hr);
            return false;
        }
        if (async_) events_ = mft_.As<IMFMediaEventGenerator>();
        if (async_ && !events_) {
            if (err) *err = "async encoder without events";
            return false;
        }
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        if (async_) {
            run_ = true;
            evThread_ = std::thread([this] { EventLoop(); });
        }
        return true;
    }

    void Close() {
        run_ = false;
        if (mft_) {
            mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        }
        if (events_) {
            // wake the event thread: shutting the MFT down ends GetEvent()
            if (auto sh = mft_.As<IMFShutdown>()) sh->Shutdown();
        }
        if (evThread_.joinable()) evThread_.join();
        events_.Reset();
        api_.Reset();
        mft_.Reset();
        async_ = false;
    }

    void SetBitrate(int bps) { SetCodecU32(api_.get(), CODECAPI_AVEncCommonMeanBitRate, (UINT32)bps); }
    void ForceKey() { forceKey_ = true; }
    bool Failed() const { return failed_; }

    // one NV12 frame (contiguous, stride = width); output goes to `out`
    void Encode(const uint8_t* nv12, LONGLONG ts, const std::function<void(const uint8_t*, size_t, bool)>& out) {
        out_ = out;
        if (async_) {
            if (needInput_.load() <= 0) return;  // the GPU is busy: drop this frame rather than queue it
            needInput_--;
        }
        DWORD size = (DWORD)(w_ * h_ * 3 / 2);
        Com<IMFSample> s = MakeSample(size);
        if (!s) return;
        Com<IMFMediaBuffer> b;
        s->GetBufferByIndex(0, &b);
        BYTE* p = nullptr;
        if (FAILED(b->Lock(&p, nullptr, nullptr))) return;
        memcpy(p, nv12, size);
        b->Unlock();
        b->SetCurrentLength(size);
        s->SetSampleTime(ts);
        s->SetSampleDuration(10000000LL / std::max(1, fps_));
        if (forceKey_.exchange(false)) SetCodecU32(api_.get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
        HRESULT hr = mft_->ProcessInput(inId_, s.get(), 0);
        if (FAILED(hr)) {
            if (hr != MF_E_NOTACCEPTING) failed_ = true;
            return;
        }
        if (!async_) Drain();
    }

private:
    void EventLoop() {
        ComInit com;
        while (run_) {
            Com<IMFMediaEvent> ev;
            if (FAILED(events_->GetEvent(0, &ev)) || !ev) break;
            MediaEventType t = MEUnknown;
            ev->GetType(&t);
            HRESULT st = S_OK;
            ev->GetStatus(&st);
            if (FAILED(st)) {
                failed_ = true;
                break;
            }
            if (t == METransformNeedInput) needInput_++;
            else if (t == METransformHaveOutput) Drain();
        }
    }

    void Drain() {
        for (;;) {
            MFT_OUTPUT_STREAM_INFO info{};
            mft_->GetOutputStreamInfo(outId_, &info);
            bool provides = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
            Com<IMFSample> s;
            if (!provides) s = MakeSample(std::max<DWORD>(info.cbSize, (DWORD)(w_ * h_ * 2)));
            MFT_OUTPUT_DATA_BUFFER ob{};
            ob.dwStreamID = outId_;
            ob.pSample = provides ? nullptr : s.get();
            DWORD status = 0;
            HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &status);
            if (ob.pEvents) ob.pEvents->Release();
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                Com<IMFMediaType> t;
                if (SUCCEEDED(mft_->GetOutputAvailableType(outId_, 0, &t))) mft_->SetOutputType(outId_, t.get(), 0);
                continue;
            }
            if (FAILED(hr)) return;
            IMFSample* got = ob.pSample;
            Com<IMFMediaBuffer> b;
            if (got && SUCCEEDED(got->ConvertToContiguousBuffer(&b))) {
                BYTE* p = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(b->Lock(&p, nullptr, &len))) {
                    Emit(p, len);
                    b->Unlock();
                }
            }
            if (provides && got) got->Release();
            if (async_) return;  // one output per HaveOutput event
        }
    }

    void Emit(const uint8_t* p, size_t n) {
        bool key = H264HasNal(p, n, 5);
        bool hasSps = H264HasNal(p, n, 7);
        if (hasSps) {
            // remember SPS/PPS (everything before the first slice) for keyframes that come without them
            size_t cut = n;
            for (size_t i = 0; i + 4 < n; i++)
                if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 && ((p[i + 3] & 0x1f) == 5 || (p[i + 3] & 0x1f) == 1)) {
                    cut = (i > 0 && p[i - 1] == 0) ? i - 1 : i;
                    break;
                }
            header_.assign(p, p + cut);
        }
        if (key && !hasSps && !header_.empty()) {
            buf_.assign(header_.begin(), header_.end());
            buf_.insert(buf_.end(), p, p + n);
            if (out_) out_(buf_.data(), buf_.size(), true);
            return;
        }
        if (out_) out_(p, n, key);
    }

    Com<IMFTransform> mft_;
    Com<ICodecAPI> api_;
    Com<IMFMediaEventGenerator> events_;
    DWORD inId_ = 0, outId_ = 0;
    int w_ = 0, h_ = 0, fps_ = 30;
    bool async_ = false;
    std::atomic<bool> run_{false}, forceKey_{false}, failed_{false};
    std::atomic<int> needInput_{0};
    std::thread evThread_;
    std::function<void(const uint8_t*, size_t, bool)> out_;
    std::vector<uint8_t> header_, buf_;
};

// ======================================================================== virtual camera control (app side)
typedef HRESULT(WINAPI* CreateVcamFn)(MFVirtualCameraType, MFVirtualCameraLifetime, MFVirtualCameraAccess, LPCWSTR, LPCWSTR,
                                      const GUID*, ULONG, IMFVirtualCamera**);
typedef HRESULT(WINAPI* VcamSupportedFn)(MFVirtualCameraType, BOOL*);

std::wstring ModuleDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&ModuleDir, &self);
    wchar_t p[MAX_PATH] = {0};
    GetModuleFileNameW(self, p, MAX_PATH);
    std::wstring s = p;
    size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : s.substr(0, k);
}

class VirtualCamera {
public:
    bool Available() {
        HMODULE m = LoadLibraryW(L"mfsensorgroup.dll");
        if (!m) return false;
        auto fn = (VcamSupportedFn)GetProcAddress(m, "MFIsVirtualCameraTypeSupported");
        create_ = (CreateVcamFn)GetProcAddress(m, "MFCreateVirtualCamera");
        BOOL ok = FALSE;
        if (!fn || !create_ || FAILED(fn(MFVirtualCameraType_SoftwareCameraSource, &ok))) return false;
        return ok != FALSE;
    }

    bool Enable(bool on, std::string* err) {
        if (!on) {
            Disable();
            return true;
        }
        if (thread_.joinable()) return true;
        if (!Available()) {
            *err = "virtual cameras need Windows 11";
            return false;
        }
        if (!EnsureRegistered(err)) return false;
        // the camera object lives on its own (MTA) thread for as long as it's enabled
        std::mutex m;
        std::condition_variable cv;
        int state = 0;
        std::string e;
        stop_ = false;
        thread_ = std::thread([&, this] {
            ComInit com;
            MFStartup(MF_VERSION, MFSTARTUP_FULL);
            IMFVirtualCamera* cam = nullptr;
            HRESULT hr = create_(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_Session, MFVirtualCameraAccess_CurrentUser,
                                 LSVC_NAME, LSVC_CLSID_STR, nullptr, 0, &cam);
            if (SUCCEEDED(hr)) hr = cam->Start(nullptr);
            {
                std::lock_guard<std::mutex> l(m);
                state = SUCCEEDED(hr) ? 1 : -1;
                if (FAILED(hr)) e = "Windows refused the virtual camera (" + Hr(hr) + ")";
            }
            cv.notify_all();
            if (SUCCEEDED(hr)) {
                std::unique_lock<std::mutex> l(stopM_);
                stopCv_.wait(l, [this] { return stop_; });
                cam->Remove();
            }
            if (cam) {
                cam->Shutdown();
                cam->Release();
            }
            MFShutdown();
        });
        std::unique_lock<std::mutex> l(m);
        cv.wait(l, [&] { return state != 0; });
        if (state < 0) {
            l.unlock();
            thread_.join();
            *err = e;
            return false;
        }
        return true;
    }

    void Disable() {
        if (!thread_.joinable()) return;
        {
            std::lock_guard<std::mutex> l(stopM_);
            stop_ = true;
        }
        stopCv_.notify_all();
        thread_.join();
        std::lock_guard<std::mutex> l(fm_);
        CloseMapping();
    }

    bool On() const { return const_cast<std::thread&>(thread_).joinable(); }

    // newest picture -> shared memory, upright and scaled down to what the camera user asked for
    void Push(const LsvImage& im) {
        if (!On()) return;
        uint64_t now = GetTickCount64();
        std::lock_guard<std::mutex> l(fm_);
        if (!view_) {
            if (now - lastOpenTry_ < 1000) return;  // nobody is using the camera yet
            lastOpenTry_ = now;
            map_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, LSVC_MAPPING);
            if (!map_) return;
            view_ = (uint8_t*)MapViewOfFile(map_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
            if (!view_) { CloseMapping(); return; }
        }
        auto* h = (LsvcHeader*)view_;
        if (h->magic != LSVC_MAGIC) return;
        if (now - lastWrite_ < 15) return;  // at most ~60 fps
        lastWrite_ = now;
        int rot = ((im.rotation % 360) + 360) % 360;
        bool swap = rot == 90 || rot == 270;
        int uw = swap ? im.height : im.width, uh = swap ? im.width : im.height;  // upright size
        int bw = h->wantW ? (int)h->wantW : 1920, bh = h->wantH ? (int)h->wantH : 1080;
        bw = std::min(bw, LSVC_MAX_W);
        bh = std::min(bh, LSVC_MAX_H);
        double f = std::min(1.0, std::min((double)bw / uw, (double)bh / uh));
        int ow = std::max(2, (int)(uw * f) & ~1), oh = std::max(2, (int)(uh * f) & ~1);
        uint8_t* dy = view_ + LSVC_DATA_OFFSET;
        uint8_t* duv = dy + (size_t)ow * oh;
        InterlockedIncrement(&h->seq);  // odd: writing
        const int W = im.width, H = im.height, ps = im.uv_pixel_stride;
        auto src = [&](int ux, int uy, int& sx, int& sy) {  // upright -> raw camera coordinates
            switch (rot) {
                case 90: sx = uy; sy = H - 1 - ux; break;
                case 180: sx = W - 1 - ux; sy = H - 1 - uy; break;
                case 270: sx = W - 1 - uy; sy = ux; break;
                default: sx = ux; sy = uy; break;
            }
        };
        for (int y = 0; y < oh; y++) {
            int uy = (int)((int64_t)y * uh / oh);
            uint8_t* row = dy + (size_t)y * ow;
            for (int x = 0; x < ow; x++) {
                int sx, sy;
                src((int)((int64_t)x * uw / ow), uy, sx, sy);
                row[x] = im.y[(size_t)sy * im.y_stride + sx];
            }
        }
        for (int y = 0; y < oh / 2; y++) {
            int uy = (int)((int64_t)(y * 2) * uh / oh);
            uint8_t* row = duv + (size_t)y * ow;
            for (int x = 0; x < ow / 2; x++) {
                int sx, sy;
                src((int)((int64_t)(x * 2) * uw / ow), uy, sx, sy);
                size_t ci = (size_t)(sy >> 1) * im.uv_stride + (size_t)(sx >> 1) * ps;
                row[x * 2] = im.u[ci];
                row[x * 2 + 1] = im.v[ci];
            }
        }
        h->width = (uint32_t)ow;
        h->height = (uint32_t)oh;
        h->writtenMs = (int64_t)now;
        InterlockedIncrement(&h->seq);  // even: done
    }

private:
    void CloseMapping() {
        if (view_) UnmapViewOfFile(view_);
        if (map_) CloseHandle(map_);
        view_ = nullptr;
        map_ = nullptr;
    }

    // The camera service can't load DLLs from user folders: copy lunashare_vcam.dll to ProgramData and register it
    // there (COM, machine-wide). Needs administrator rights once per version.
    bool EnsureRegistered(std::string* err) {
        std::wstring srcPath = ModuleDir() + L"\\lunashare_vcam.dll";
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!GetFileAttributesExW(srcPath.c_str(), GetFileExInfoStandard, &fa)) {
            *err = "lunashare_vcam.dll is missing next to the video module";
            return false;
        }
        wchar_t pd[MAX_PATH] = L"C:\\ProgramData";
        GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH);
        wchar_t stamp[64];
        swprintf(stamp, 64, L"%08lx%08lx", (unsigned long)fa.ftLastWriteTime.dwHighDateTime, (unsigned long)fa.nFileSizeLow);
        std::wstring dir = std::wstring(pd) + L"\\LunaShare";
        std::wstring dst = dir + L"\\lunashare_vcam_" + stamp + L".dll";
        if (RegisteredPath() == dst && GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
        std::wstring args = L"/c mkdir \"" + dir + L"\" 2>nul & copy /y \"" + srcPath + L"\" \"" + dst + L"\" >nul && regsvr32 /s \"" + dst + L"\"";
        SHELLEXECUTEINFOW sei{sizeof(sei)};
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE;
        sei.lpVerb = L"runas";
        sei.lpFile = L"cmd.exe";
        sei.lpParameters = args.c_str();
        sei.nShow = SW_HIDE;
        if (!ShellExecuteExW(&sei) || !sei.hProcess) {
            *err = GetLastError() == ERROR_CANCELLED ? "the one-time setup needs administrator permission (it was declined)"
                                                     : "could not run the one-time setup";
            return false;
        }
        WaitForSingleObject(sei.hProcess, 60000);
        CloseHandle(sei.hProcess);
        if (RegisteredPath() != dst) {
            *err = "the one-time setup did not finish (registration failed)";
            return false;
        }
        return true;
    }

    static std::wstring RegisteredPath() {
        std::wstring key = std::wstring(L"Software\\Classes\\CLSID\\") + LSVC_CLSID_STR + L"\\InprocServer32";
        wchar_t buf[MAX_PATH * 2] = {0};
        DWORD size = sizeof(buf);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
        return buf;
    }

    CreateVcamFn create_ = nullptr;
    std::thread thread_;
    std::mutex stopM_, fm_;
    std::condition_variable stopCv_;
    bool stop_ = false;
    HANDLE map_ = nullptr;
    uint8_t* view_ = nullptr;
    uint64_t lastOpenTry_ = 0, lastWrite_ = 0;
};

// ======================================================================== backend
class WinBackend : public Backend {
public:
    WinBackend(Sink* s) : sink_(s) {
        ComInit com;
        mfOk_ = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL));
    }
    ~WinBackend() override {
        vcam_.Disable();
        StopCamera();
        StopDecoder();
    }

    std::vector<LsvCamera> Cameras() override {
        ComInit com;
        std::vector<LsvCamera> out;
        Com<IMFAttributes> a;
        if (FAILED(MFCreateAttributes(&a, 1))) return out;
        a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        IMFActivate** devs = nullptr;
        UINT32 n = 0;
        if (FAILED(MFEnumDeviceSources(a.get(), &devs, &n))) return out;
        for (UINT32 i = 0; i < n; i++) {
            WCHAR *name = nullptr, *link = nullptr;
            UINT32 len = 0;
            devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len);
            devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &len);
            LsvCamera c{};
            std::string nm = Narrow(name), id = Narrow(link);
            snprintf(c.name, sizeof(c.name), "%s", nm.empty() ? "Camera" : nm.c_str());
            snprintf(c.id, sizeof(c.id), "%u", i);  // symbolic links are too long for the id field; index + lookup
            std::string lower = nm;
            for (auto& ch : lower) ch = (char)tolower((unsigned char)ch);
            c.facing = lower.find("front") != std::string::npos ? LSV_FACING_FRONT
                       : (lower.find("rear") != std::string::npos || lower.find("back") != std::string::npos) ? LSV_FACING_BACK
                                                                                                                : LSV_FACING_EXTERNAL;
            links_[i % 16] = id;
            out.push_back(c);
            if (name) CoTaskMemFree(name);
            if (link) CoTaskMemFree(link);
            devs[i]->Release();
        }
        CoTaskMemFree(devs);
        {  // the screen: shared like a camera
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
        if (!mfOk_) {
            if (err) *err = "Media Foundation unavailable";
            return false;
        }
        if (id == "screen") return StartScreen(s, err);
        Cameras();  // refresh links
        unsigned idx = (unsigned)atoi(id.c_str());
        std::string link = links_[idx % 16];
        if (link.empty()) {
            if (err) *err = "camera not found";
            return false;
        }
        s_ = s;
        bitrate_ = s.bitrate;
        boxW_ = boxH_ = encFps_ = 0;
        gate_ = FrameGate();
        run_ = true;
        std::mutex wm;
        std::condition_variable wcv;
        int state = 0;  // 1 ok, -1 failed
        std::string startErr;
        th_ = std::thread([&, link] {
            ComInit com;
            bool ok = OpenCamera(link, &startErr);
            {
                std::lock_guard<std::mutex> l(wm);
                state = ok ? 1 : -1;
            }
            wcv.notify_all();
            if (ok) CaptureLoop();
            CloseCamera();
        });
        std::unique_lock<std::mutex> l(wm);
        wcv.wait(l, [&] { return state != 0; });
        if (state < 0) {
            l.unlock();
            run_ = false;
            th_.join();
            if (err) *err = startErr;
            return false;
        }
        return true;
    }

    void StopCamera() override {
        run_ = false;
        if (th_.joinable()) th_.join();
    }

    // smaller pictures: scaled down in front of the encoder (reopened at the new size), the camera keeps running.
    // (The screen keeps its size: Rescale is then simply a no-op.)
    bool ScalesDown() const override { return true; }
    bool Rescale(const LsvSettings& s) override {
        if (!run_) return false;
        boxW_ = s.width;
        boxH_ = s.height;
        encFps_ = s.fps;
        bitrate_ = s.bitrate;
        newBitrate_ = true;
        keyReq_ = true;
        return true;
    }

    void SetBitrate(int bps) override {
        bitrate_ = bps;
        newBitrate_ = true;
    }
    void RequestKeyframe() override { keyReq_ = true; }

    std::vector<LsvControl> Controls() override {
        std::lock_guard<std::mutex> l(ctlM_);
        std::vector<LsvControl> out;
        if (!cam_ && !amp_) return out;
        auto add = [&](int id, IAMCameraControl* c, long prop, bool autoCtl) {
            long mn, mx, step, def, caps, val, flags;
            if (FAILED(c->GetRange(prop, &mn, &mx, &step, &def, &caps)) || mx <= mn) return;
            if (FAILED(c->Get(prop, &val, &flags))) val = def, flags = caps;
            if (autoCtl) {
                if ((caps & CameraControl_Flags_Auto) && (caps & CameraControl_Flags_Manual))
                    out.push_back({id, 0, 1, 1, (flags & CameraControl_Flags_Auto) ? 1.f : 0.f});
                return;
            }
            out.push_back({id, (float)mn, (float)mx, (float)std::max(1L, step), (float)val});
        };
        if (cam_) {
            add(LSV_CTL_ZOOM, cam_.get(), CameraControl_Zoom, false);
            add(LSV_CTL_AUTO_FOCUS, cam_.get(), CameraControl_Focus, true);
            add(LSV_CTL_FOCUS, cam_.get(), CameraControl_Focus, false);
            add(LSV_CTL_AUTO_EXPOSURE, cam_.get(), CameraControl_Exposure, true);
            // exposure is log2(seconds): show it as a shutter time in milliseconds
            long mn, mx, step, def, caps, val, flags;
            if (SUCCEEDED(cam_->GetRange(CameraControl_Exposure, &mn, &mx, &step, &def, &caps)) && mx > mn) {
                if (FAILED(cam_->Get(CameraControl_Exposure, &val, &flags))) val = def;
                out.push_back({LSV_CTL_SHUTTER, (float)(std::pow(2.0, mn) * 1000), (float)(std::pow(2.0, mx) * 1000), 0,
                               (float)(std::pow(2.0, val) * 1000)});
            }
        }
        if (amp_) {
            long mn, mx, step, def, caps, val, flags;
            if (SUCCEEDED(amp_->GetRange(VideoProcAmp_Brightness, &mn, &mx, &step, &def, &caps)) && mx > mn) {
                if (FAILED(amp_->Get(VideoProcAmp_Brightness, &val, &flags))) val = def;
                out.push_back({LSV_CTL_BRIGHTNESS, (float)mn, (float)mx, (float)std::max(1L, step), (float)val});
            }
        }
        return out;
    }

    void SetControl(int id, float v) override {
        std::lock_guard<std::mutex> l(ctlM_);
        auto setCam = [&](long prop, long val, long flags) { if (cam_) cam_->Set(prop, val, flags); };
        long cur = 0, flags = 0;
        switch (id) {
            case LSV_CTL_ZOOM: setCam(CameraControl_Zoom, std::lround(v), CameraControl_Flags_Manual); break;
            case LSV_CTL_AUTO_FOCUS:
                if (cam_ && SUCCEEDED(cam_->Get(CameraControl_Focus, &cur, &flags)))
                    cam_->Set(CameraControl_Focus, cur, v > 0.5f ? CameraControl_Flags_Auto : CameraControl_Flags_Manual);
                break;
            case LSV_CTL_FOCUS: setCam(CameraControl_Focus, std::lround(v), CameraControl_Flags_Manual); break;
            case LSV_CTL_AUTO_EXPOSURE:
                if (cam_ && SUCCEEDED(cam_->Get(CameraControl_Exposure, &cur, &flags)))
                    cam_->Set(CameraControl_Exposure, cur, v > 0.5f ? CameraControl_Flags_Auto : CameraControl_Flags_Manual);
                break;
            case LSV_CTL_SHUTTER:
                setCam(CameraControl_Exposure, std::lround(std::log2(std::max(0.01, (double)v / 1000.0))), CameraControl_Flags_Manual);
                break;
            case LSV_CTL_BRIGHTNESS:
                if (amp_) amp_->Set(VideoProcAmp_Brightness, std::lround(v), VideoProcAmp_Flags_Manual);
                break;
            default: break;
        }
    }

    // ---------------------------------------------------------------- decoder (core's decode thread)
    bool StartDecoder() override {
        decCom_ = std::make_unique<ComInit>();
        if (!mfOk_) return false;
        HRESULT hr = CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dec_));
        if (FAILED(hr)) {
            sink_->Log("H.264 decoder: " + Hr(hr));
            return false;
        }
        if (auto api = dec_.As<ICodecAPI>()) SetCodecBool(api.get(), CODECAPI_AVLowLatencyMode, true);
        decGpu_ = EnableGpuDecoding();
        Com<IMFMediaType> it;
        MFCreateMediaType(&it);
        it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        it->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        it->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (FAILED(hr = dec_->SetInputType(0, it.get(), 0))) {
            sink_->Log("H.264 decoder input " + Hr(hr));
            dec_.Reset();
            return false;
        }
        SetDecoderOutput();
        dec_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        dec_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    // GPU decode profiles of this PC's graphics card (D3D11 video decoder)
    static bool GpuHasProfile(const GUID& profile) {
        Com<ID3D11Device> dev;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                                     D3D11_SDK_VERSION, &dev, nullptr, nullptr)))
            return false;
        Com<ID3D11VideoDevice> vd = dev.As<ID3D11VideoDevice>();
        if (!vd) return false;
        UINT n = vd->GetVideoDecoderProfileCount();
        for (UINT i = 0; i < n; i++) {
            GUID g;
            if (SUCCEEDED(vd->GetVideoDecoderProfile(i, &g)) && g == profile) return true;
        }
        return false;
    }

    unsigned EncodeCodecs() override { return CodecBit(kH264); }
    unsigned DecodeCodecs() override {
        if (decMask_) return decMask_;
        unsigned m = CodecBit(kH264);
        std::string err;
        if (FfDecoder::Load({Narrow(ModuleDir().c_str())}, &err)) {
            // HEVC: on the GPU when it can, else on the CPU (fine up to ~4K30); AV1 only with a GPU decoder
            static const GUID kAv1 = {0xb8be4ccb, 0xcf53, 0x46ba, {0x8d, 0x59, 0xd6, 0xb8, 0xa6, 0xda, 0x5d, 0x2a}};
            m |= CodecBit(kHEVC);
            if (GpuHasProfile(kAv1)) m |= CodecBit(kAV1);
        } else {
            sink_->Log("no HEVC/AV1: " + err);
        }
        decMask_ = m;
        return m;
    }

    void Decode(const uint8_t* d, size_t n, bool key, int w, int h, int codec) override {
        if (codec != kH264) {
            DecodeFf(d, n, key, w, h, codec);
            return;
        }
        if (!dec_) return;
        dispW_ = w;
        dispH_ = h;
        Com<IMFSample> s = MakeSample((DWORD)n);
        if (!s) return;
        Com<IMFMediaBuffer> b;
        s->GetBufferByIndex(0, &b);
        BYTE* p = nullptr;
        b->Lock(&p, nullptr, nullptr);
        memcpy(p, d, n);
        b->Unlock();
        b->SetCurrentLength((DWORD)n);
        s->SetSampleTime(decTs_);
        decTs_ += 333333;
        HRESULT hr = dec_->ProcessInput(0, s.get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            DrainDecoder();
            hr = dec_->ProcessInput(0, s.get(), 0);
        }
        if (FAILED(hr)) {
            sink_->OnDecoderError();
            return;
        }
        DrainDecoder();
    }

    void DecodeFf(const uint8_t* d, size_t n, bool key, int w, int h, int codec) {
        if (!ff_.IsOpen() || ff_.OpenCodec() != codec) {
            if (!key) {
                sink_->OnDecoderError();
                return;
            }
            std::string err;
            if (!ff_.Open(codec, true, &err)) {
                sink_->Log("decoder: " + err);
                sink_->OnDecoderError();
                return;
            }
        }
        dispW_ = w;
        dispH_ = h;
        if (!ff_.Decode(d, n, [&](const LsvImage& im) { sink_->OnDecoded(im); })) sink_->OnDecoderError();
    }

    void StopDecoder() override {
        ff_.Close();
        if (dec_) {
            dec_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            if (dxgiMgr_) dec_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
            dec_.Reset();
        }
        dxgiMgr_.Reset();
        d3d_.Reset();
        decGpu_ = false;
        decCom_.reset();
    }

    std::string EncoderName() override {
        std::lock_guard<std::mutex> l(nameM_);
        return encName_;
    }
    bool VcamAvailable() override { return vcam_.Available(); }
    bool VcamEnable(bool on, std::string* err) override { return vcam_.Enable(on, err); }
    void VcamPush(const LsvImage& img) override { vcam_.Push(img); }

    std::string DecoderName() override {
        if (ff_.IsOpen()) return ff_.Name();
        return dec_ ? (decGpu_ ? "Microsoft H.264 decoder (GPU)" : "Microsoft H.264 decoder (software)") : "";
    }

private:
    // ---------------------------------------------------------------- capture
    bool OpenCamera(const std::string& link, std::string* err) {
        Com<IMFAttributes> a;
        MFCreateAttributes(&a, 2);
        a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        std::wstring wl = Wide(link);
        a->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, wl.c_str());
        HRESULT hr = MFCreateDeviceSource(a.get(), &source_);
        if (FAILED(hr)) {
            *err = hr == E_ACCESSDENIED ? "camera access denied (Windows privacy settings)" : "cannot open camera " + Hr(hr);
            return false;
        }
        Com<IMFAttributes> ra;
        MFCreateAttributes(&ra, 2);
        ra->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        ra->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        if (FAILED(hr = MFCreateSourceReaderFromMediaSource(source_.get(), ra.get(), &reader_))) {
            *err = "camera reader " + Hr(hr);
            return false;
        }
        // the camera mode closest to what we want (prefer >= the requested frame rate)
        const DWORD stream = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
        Com<IMFMediaType> best;
        double bestScore = 1e18;
        UINT32 bw = 0, bh = 0;
        for (DWORD i = 0;; i++) {
            Com<IMFMediaType> t;
            if (FAILED(reader_->GetNativeMediaType(stream, i, &t))) break;
            UINT32 w = 0, h = 0, fn = 0, fd = 1;
            MFGetAttributeSize(t.get(), MF_MT_FRAME_SIZE, &w, &h);
            MFGetAttributeRatio(t.get(), MF_MT_FRAME_RATE, &fn, &fd);
            if (!w || !h) continue;
            double fps = fd ? (double)fn / fd : 0;
            double score = std::abs((double)h - s_.height) * 4 + std::abs((double)w - s_.width) + (fps + 0.5 < s_.fps ? 2000 : 0) +
                           (h < (UINT32)s_.height ? 500 : 0);
            if (score < bestScore) {
                bestScore = score;
                best = t;
                bw = w;
                bh = h;
            }
        }
        if (best) reader_->SetCurrentMediaType(stream, nullptr, best.get());
        if (!bw) bw = (UINT32)s_.width, bh = (UINT32)s_.height;
        // output: fit into the requested box, keep the camera's aspect ratio, even sizes
        double scale = std::min(1.0, std::min((double)s_.width / bw, (double)s_.height / bh));
        if ((double)bh * scale < s_.height * 0.9 && bh >= (UINT32)s_.height) scale = (double)s_.height / bh;
        w_ = std::max(16, (int)(bw * scale) & ~15);
        h_ = std::max(16, (int)(bh * scale) & ~1);
        Com<IMFMediaType> out;
        MFCreateMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(out.get(), MF_MT_FRAME_SIZE, (UINT32)w_, (UINT32)h_);
        MFSetAttributeRatio(out.get(), MF_MT_FRAME_RATE, (UINT32)s_.fps, 1);
        if (FAILED(hr = reader_->SetCurrentMediaType(stream, nullptr, out.get()))) {
            // no scaler: take the native size
            w_ = (int)bw & ~15;
            h_ = (int)bh & ~1;
            MFSetAttributeSize(out.get(), MF_MT_FRAME_SIZE, (UINT32)w_, (UINT32)h_);
            if (FAILED(hr = reader_->SetCurrentMediaType(stream, nullptr, out.get()))) {
                *err = "camera does not deliver NV12 " + Hr(hr);
                return false;
            }
        }
        {
            std::lock_guard<std::mutex> l(ctlM_);
            cam_ = source_.As<IAMCameraControl>();
            amp_ = source_.As<IAMVideoProcAmp>();
        }
        std::string e;
        encW_ = encH_ = 0;  // (the camera's size)
        if (!OpenEncoder(&e)) {
            *err = e;
            return false;
        }
        return true;
    }

    bool OpenEncoder(std::string* err, int w = 0, int h = 0, int fps = 0) {
        if (!w) w = encW_ ? encW_ : w_, h = encW_ ? encH_ : h_;
        if (!fps) fps = s_.fps;
        encW_ = w;
        encH_ = h;
        enc_ = std::make_unique<Encoder>();
        std::string e1;
        if (!triedSoftwareOnly_ && enc_->Open(w, h, fps, bitrate_, true, &e1)) {
            SetEncName(enc_->name + " (hardware)");
            return true;
        }
        enc_->Close();
        enc_ = std::make_unique<Encoder>();
        std::string e2;
        if (enc_->Open(w, h, fps, bitrate_, false, &e2)) {
            SetEncName(enc_->name.empty() ? "H.264 (software)" : enc_->name);
            return true;
        }
        enc_.reset();
        *err = e2.empty() ? e1 : e2;
        return false;
    }
    void SetEncName(const std::string& n) {
        std::lock_guard<std::mutex> l(nameM_);
        encName_ = n;
    }

    // the screen instead of a camera: grabbed ten times a second, through the same encoder
    bool StartScreen(const LsvSettings& s, std::string* err) {
        s_ = s;
        s_.fps = 10;
        bitrate_ = std::max(s.bitrate, 1500000);
        run_ = true;
        std::mutex wm;
        std::condition_variable wcv;
        int state = 0;
        std::string startErr;
        th_ = std::thread([&] {
            ComInit com;
            ScreenGrab grab;
            bool ok = grab.Open(&startErr);
            if (ok) {
                w_ = grab.Width();
                h_ = grab.Height();
                encW_ = encH_ = 0;
                ok = OpenEncoder(&startErr);
            }
            {
                std::lock_guard<std::mutex> l(wm);
                state = ok ? 1 : -1;
            }
            wcv.notify_all();
            if (ok) ScreenLoop(grab);
            CloseCamera();
        });
        std::unique_lock<std::mutex> l(wm);
        wcv.wait(l, [&] { return state != 0; });
        if (state < 0) {
            l.unlock();
            run_ = false;
            th_.join();
            if (err) *err = startErr;
            return false;
        }
        return true;
    }

    void ScreenLoop(ScreenGrab& grab) {
        std::vector<uint8_t> frame;
        LONGLONG ts = 0;
        auto next = std::chrono::steady_clock::now();
        int misses = 0;
        while (run_) {
            next += std::chrono::milliseconds(100);
            std::this_thread::sleep_until(next);
            if (!grab.Grab(frame, 2)) {
                if (++misses > 50) {
                    sink_->OnCameraError("the screen can't be read");
                    break;
                }
                continue;
            }
            misses = 0;
            LsvImage img{w_, h_, frame.data(), frame.data() + (size_t)w_ * h_, frame.data() + (size_t)w_ * h_ + 1, w_, w_, 2, 0, 0};
            sink_->OnLocalImage(img);  // (not mirrored: it's a screen)
            if (!enc_) continue;
            if (newBitrate_.exchange(false)) enc_->SetBitrate(bitrate_);
            if (keyReq_.exchange(false)) enc_->ForceKey();
            enc_->Encode(frame.data(), ts, [&](const uint8_t* d, size_t n, bool key) {
                EncodedFrame f;
                f.data = d;
                f.len = n;
                f.key = key;
                f.width = w_;
                f.height = h_;
                f.codec = kH264;
                sink_->OnEncoded(f);
            });
            ts += 1000000;  // (100 ms in 100 ns units)
            if (enc_->Failed()) {
                sink_->Log("hardware encoder failed, switching to software");
                enc_->Close();
                triedSoftwareOnly_ = true;
                std::string e;
                if (!OpenEncoder(&e)) {
                    sink_->OnCameraError("no working H.264 encoder: " + e);
                    break;
                }
                keyReq_ = true;
            }
        }
    }

    void CaptureLoop() {
        std::vector<uint8_t> frame((size_t)w_ * h_ * 3 / 2);
        while (run_) {
            DWORD idx = 0, flags = 0;
            LONGLONG ts = 0;
            Com<IMFSample> s;
            HRESULT hr = reader_->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &idx, &flags, &ts, &s);
            if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
                sink_->OnCameraError("camera stopped " + Hr(hr));
                break;
            }
            if (!s || !run_) continue;
            Com<IMFMediaBuffer> b;
            if (FAILED(s->GetBufferByIndex(0, &b))) continue;
            BYTE* p = nullptr;
            LONG pitch = w_;
            bool locked2d = false;
            Com<IMF2DBuffer> b2 = b.As<IMF2DBuffer>();
            if (b2 && SUCCEEDED(b2->Lock2D(&p, &pitch))) locked2d = true;
            else if (FAILED(b->Lock(&p, nullptr, nullptr))) continue;
            // copy into a tight NV12 frame (the encoder wants stride == width)
            for (int y = 0; y < h_; y++) memcpy(&frame[(size_t)y * w_], p + (size_t)y * pitch, (size_t)w_);
            const uint8_t* uvSrc = p + (size_t)pitch * h_;
            for (int y = 0; y < h_ / 2; y++) memcpy(&frame[(size_t)w_ * h_ + (size_t)y * w_], uvSrc + (size_t)y * pitch, (size_t)w_);
            if (locked2d) b2->Unlock2D();
            else b->Unlock();

            LsvImage img{w_, h_, frame.data(), frame.data() + (size_t)w_ * h_, frame.data() + (size_t)w_ * h_ + 1, w_, w_, 2, 0, 1};
            sink_->OnLocalImage(img);  // webcams: mirrored self-view like a mirror

            if (!enc_) continue;
            int64_t nowMs = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (!gate_.Pass(encFps_, nowMs)) continue;  // (a lower frame rate than the camera's)
            int ew, eh;
            FitInBox(w_, h_, boxW_, boxH_, 16, ew, eh);
            if (ew != encW_ || eh != encH_) {  // the size changed: a new encoder for it
                enc_->Close();
                std::string e;
                if (!OpenEncoder(&e, ew, eh, encFps_ > 0 ? (int)encFps_ : s_.fps)) {
                    sink_->Log("encoder at " + std::to_string(ew) + "x" + std::to_string(eh) + " failed (" + e + "), back to full size");
                    boxW_ = boxH_ = 0;
                    if (!OpenEncoder(&e, w_, h_, s_.fps)) {
                        sink_->OnCameraError("no working H.264 encoder: " + e);
                        break;
                    }
                }
                keyReq_ = true;
            }
            const uint8_t* encData = frame.data();
            if (encW_ != w_ || encH_ != h_) {
                img.mirror = 0;
                ScaleYuv420(img, encW_, encH_, true, scaled_);
                encData = scaled_.data();
            }
            if (newBitrate_.exchange(false)) enc_->SetBitrate(bitrate_);
            if (keyReq_.exchange(false)) enc_->ForceKey();
            enc_->Encode(encData, ts, [&](const uint8_t* d, size_t n, bool key) {
                EncodedFrame f;
                f.data = d;
                f.len = n;
                f.key = key;
                f.width = encW_;
                f.height = encH_;
                f.codec = kH264;
                sink_->OnEncoded(f);
            });
            if (enc_->Failed()) {
                // a GPU encoder that won't take our frames: continue with the software one
                sink_->Log("hardware encoder failed, switching to software");
                enc_->Close();
                triedSoftwareOnly_ = true;
                std::string e;
                if (!OpenEncoder(&e)) {
                    sink_->OnCameraError("no working H.264 encoder: " + e);
                    break;
                }
                keyReq_ = true;
            }
        }
    }

    void CloseCamera() {
        if (enc_) enc_->Close();
        enc_.reset();
        {
            std::lock_guard<std::mutex> l(ctlM_);
            cam_.Reset();
            amp_.Reset();
        }
        reader_.Reset();
        if (source_) source_->Shutdown();
        source_.Reset();
    }

    // ---------------------------------------------------------------- decoding
    // Hardware decoding (DXVA) through a D3D11 device: needed for 4K / high frame rates. Frames are read back to
    // system memory when locked, so the rest of the pipeline stays the same.
    bool EnableGpuDecoding() {
        Com<IMFAttributes> a;
        UINT32 aware = 0;
        if (FAILED(dec_->GetAttributes(&a)) || !a || FAILED(a->GetUINT32(MF_SA_D3D11_AWARE, &aware)) || !aware) return false;
        D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels,
                                       (UINT)(sizeof(levels) / sizeof(levels[0])), D3D11_SDK_VERSION, &d3d_, nullptr, nullptr);
        if (FAILED(hr)) return false;
        if (auto mt = d3d_.As<ID3D10Multithread>()) mt->SetMultithreadProtected(TRUE);
        UINT token = 0;
        if (FAILED(MFCreateDXGIDeviceManager(&token, &dxgiMgr_)) || FAILED(dxgiMgr_->ResetDevice(d3d_.get(), token))) {
            dxgiMgr_.Reset();
            d3d_.Reset();
            return false;
        }
        if (FAILED(dec_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)dxgiMgr_.get()))) {
            dxgiMgr_.Reset();
            d3d_.Reset();
            return false;
        }
        return true;
    }

    void SetDecoderOutput() {
        for (DWORD i = 0;; i++) {
            Com<IMFMediaType> t;
            if (FAILED(dec_->GetOutputAvailableType(0, i, &t))) break;
            GUID sub{};
            t->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub == MFVideoFormat_NV12) {
                if (SUCCEEDED(dec_->SetOutputType(0, t.get(), 0))) {
                    UINT32 w = 0, h = 0;
                    MFGetAttributeSize(t.get(), MF_MT_FRAME_SIZE, &w, &h);
                    UINT32 stride = 0;
                    if (FAILED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) stride = w;
                    outW_ = (int)w;
                    outH_ = (int)h;
                    outStride_ = (int)stride;
                }
                return;
            }
        }
    }

    void DrainDecoder() {
        for (;;) {
            MFT_OUTPUT_STREAM_INFO info{};
            dec_->GetOutputStreamInfo(0, &info);
            bool provides = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | (decGpu_ ? MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES : 0))) != 0;
            Com<IMFSample> s;
            if (!provides) s = MakeSample(std::max<DWORD>(info.cbSize, (DWORD)(std::max(outW_, 16) * std::max(outH_, 16) * 3 / 2)));
            MFT_OUTPUT_DATA_BUFFER ob{};
            ob.pSample = provides ? nullptr : s.get();
            DWORD status = 0;
            HRESULT hr = dec_->ProcessOutput(0, 1, &ob, &status);
            if (ob.pEvents) ob.pEvents->Release();
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                SetDecoderOutput();
                continue;
            }
            if (FAILED(hr)) {
                sink_->OnDecoderError();
                return;
            }
            IMFSample* got = ob.pSample;
            Com<IMFMediaBuffer> b;
            if (got && SUCCEEDED(got->GetBufferByIndex(0, &b))) {
                BYTE* p = nullptr;
                LONG pitch = outStride_ > 0 ? outStride_ : outW_;
                Com<IMF2DBuffer> b2 = b.As<IMF2DBuffer>();
                bool locked2d = b2 && SUCCEEDED(b2->Lock2D(&p, &pitch));
                if (locked2d || SUCCEEDED(b->Lock(&p, nullptr, nullptr))) {
                    int w = std::min(dispW_ > 0 ? dispW_ : outW_, outW_), h = std::min(dispH_ > 0 ? dispH_ : outH_, outH_);
                    const uint8_t* uv = p + (size_t)pitch * outH_;  // the UV plane follows the (aligned) luma plane
                    LsvImage img{w, h, p, uv, uv + 1, (int)pitch, (int)pitch, 2, 0, 0};
                    sink_->OnDecoded(img);
                    if (locked2d) b2->Unlock2D();
                    else b->Unlock();
                }
            }
            if (provides && got) got->Release();
        }
    }

    Sink* sink_;
    bool mfOk_ = false;
    std::string links_[16];
    LsvSettings s_{};
    std::atomic<int> bitrate_{0};
    std::atomic<bool> run_{false}, newBitrate_{false}, keyReq_{false};
    bool triedSoftwareOnly_ = false;
    std::thread th_;
    int w_ = 0, h_ = 0;
    int encW_ = 0, encH_ = 0;                          // the size the encoder is open at
    std::atomic<int> boxW_{0}, boxH_{0}, encFps_{0};   // Rescale(): the box to fit the sent picture into, its frame rate
    FrameGate gate_;
    std::vector<uint8_t> scaled_;
    Com<IMFMediaSource> source_;
    Com<IMFSourceReader> reader_;
    std::mutex ctlM_, nameM_;
    Com<IAMCameraControl> cam_;
    Com<IAMVideoProcAmp> amp_;
    std::unique_ptr<Encoder> enc_;
    std::string encName_;
    // decoder
    std::unique_ptr<ComInit> decCom_;
    VirtualCamera vcam_;
    FfDecoder ff_;
    unsigned decMask_ = 0;
    Com<IMFTransform> dec_;
    Com<ID3D11Device> d3d_;
    Com<IMFDXGIDeviceManager> dxgiMgr_;
    bool decGpu_ = false;
    int outW_ = 0, outH_ = 0, outStride_ = 0, dispW_ = 0, dispH_ = 0;
    LONGLONG decTs_ = 0;
};

}  // namespace

std::unique_ptr<Backend> CreateBackend(Sink* sink, const LsvHost*) { return std::make_unique<WinBackend>(sink); }

}  // namespace lsv
