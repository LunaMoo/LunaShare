// SPDX-License-Identifier: GPL-3.0-only
#include "mp4_writer.h"

#include <algorithm>
#include <cstring>

#include "platform.h"

namespace ls {
namespace {

constexpr uint32_t kVideoScale = 90000, kAudioScale = 48000, kOpusFrame = 960;

struct Box {
    std::vector<uint8_t> b;
    void u8(uint32_t v) { b.push_back((uint8_t)v); }
    void u16(uint32_t v) { u8(v >> 8); u8(v); }
    void u32(uint32_t v) { u16(v >> 16); u16(v & 0xffff); }
    void u64(uint64_t v) { u32((uint32_t)(v >> 32)); u32((uint32_t)v); }
    void zeros(size_t n) { b.insert(b.end(), n, 0); }
    void str(const char* s, size_t n) { b.insert(b.end(), s, s + n); }
    void add(const Box& o) { b.insert(b.end(), o.b.begin(), o.b.end()); }
    void bytes(const std::vector<uint8_t>& v) { b.insert(b.end(), v.begin(), v.end()); }
    // wraps the content into a box of the given type
    Box wrap(const char* type) const {
        Box o;
        o.u32((uint32_t)(b.size() + 8));
        o.str(type, 4);
        o.add(*this);
        return o;
    }
    Box full(const char* type, uint32_t versionFlags) const {
        Box o;
        o.u32(versionFlags);
        o.add(*this);
        return o.wrap(type);
    }
};

void Matrix(Box& b, int rotation) {
    // a b u / c d v / x y w, a..d and x,y in 16.16, u v w in 2.30. Same values FFmpeg writes for its rotate tag.
    int32_t a = 0x10000, bb = 0, c = 0, d = 0x10000;
    switch (rotation) {
        case 90: a = 0, bb = 0x10000, c = -0x10000, d = 0; break;
        case 180: a = -0x10000, d = -0x10000; break;
        case 270: a = 0, bb = -0x10000, c = 0x10000, d = 0; break;
        default: break;
    }
    b.u32((uint32_t)a); b.u32((uint32_t)bb); b.u32(0);
    b.u32((uint32_t)c); b.u32((uint32_t)d); b.u32(0);
    b.u32(0); b.u32(0); b.u32(0x40000000);
}

// splits Annex B into NAL units (without start codes)
template <class F>
void ForEachNal(const uint8_t* p, size_t n, F f) {
    size_t i = 0, start = SIZE_MAX;
    while (i + 2 < n) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            if (start != SIZE_MAX) {
                size_t end = i;
                while (end > start && p[end - 1] == 0) end--;  // the zero of a 4-byte start code / trailing zeros
                if (end > start) f(p + start, end - start);
            }
            i += 3;
            start = i;
        } else {
            i++;
        }
    }
    if (start != SIZE_MAX && start < n) f(p + start, n - start);
}

Box Stbl(const Box& stsd, const std::vector<uint32_t>& deltas, const std::vector<uint32_t>* keys,
         const std::vector<uint32_t>& sizes, const std::vector<uint64_t>& offsets) {
    Box stts;
    std::vector<std::pair<uint32_t, uint32_t>> runs;
    for (uint32_t d : deltas) {
        if (!runs.empty() && runs.back().second == d) runs.back().first++;
        else runs.push_back({1, d});
    }
    stts.u32((uint32_t)runs.size());
    for (auto& r : runs) { stts.u32(r.first); stts.u32(r.second); }
    Box stsc;
    stsc.u32(1);
    stsc.u32(1); stsc.u32(1); stsc.u32(1);  // every chunk holds one sample
    Box stsz;
    stsz.u32(0);
    stsz.u32((uint32_t)sizes.size());
    for (uint32_t s : sizes) stsz.u32(s);
    Box co64;
    co64.u32((uint32_t)offsets.size());
    for (uint64_t o : offsets) co64.u64(o);
    Box stbl;
    stbl.add(stsd);
    stbl.add(stts.full("stts", 0));
    if (keys) {
        Box stss;
        stss.u32((uint32_t)keys->size());
        for (uint32_t k : *keys) stss.u32(k);
        stbl.add(stss.full("stss", 0));
    }
    stbl.add(stsc.full("stsc", 0));
    stbl.add(stsz.full("stsz", 0));
    stbl.add(co64.full("co64", 0));
    return stbl.wrap("stbl");
}

Box Trak(uint32_t id, bool video, uint64_t movieDur, uint32_t scale, uint64_t mediaDur, int rotation, int w, int h,
         const Box& stbl) {
    Box tkhd;
    tkhd.u32(0); tkhd.u32(0);  // creation / modification
    tkhd.u32(id);
    tkhd.u32(0);
    tkhd.u32((uint32_t)movieDur);
    tkhd.zeros(8);
    tkhd.u16(0); tkhd.u16(0);  // layer, alternate group
    tkhd.u16(video ? 0 : 0x0100);
    tkhd.u16(0);
    Matrix(tkhd, video ? rotation : 0);
    tkhd.u32(video ? (uint32_t)w << 16 : 0);
    tkhd.u32(video ? (uint32_t)h << 16 : 0);

    Box mdhd;
    mdhd.u32(0); mdhd.u32(0);
    mdhd.u32(scale);
    mdhd.u32((uint32_t)mediaDur);
    mdhd.u16(0x55c4);  // "und"
    mdhd.u16(0);
    Box hdlr;
    hdlr.u32(0);
    hdlr.str(video ? "vide" : "soun", 4);
    hdlr.zeros(12);
    const char* name = video ? "VideoHandler" : "SoundHandler";
    hdlr.str(name, strlen(name) + 1);
    Box header;
    if (video) header.zeros(8);  // graphicsmode + opcolor
    else header.zeros(4);        // balance + reserved
    Box url;
    Box dref;
    dref.u32(1);
    dref.add(url.full("url ", 1));
    Box dinf = dref.full("dref", 0).wrap("dinf");
    Box minf;
    minf.add(video ? header.full("vmhd", 1) : header.full("smhd", 0));
    minf.add(dinf);
    minf.add(stbl);
    Box mdia;
    mdia.add(mdhd.full("mdhd", 0));
    mdia.add(hdlr.full("hdlr", 0));
    mdia.add(minf.wrap("minf"));
    Box trak;
    trak.add(tkhd.full("tkhd", 3));
    trak.add(mdia.wrap("mdia"));
    return trak.wrap("trak");
}

}  // namespace

Mp4Writer::~Mp4Writer() { Abort(); }

void Mp4Writer::Write(const void* d, size_t n) {
    if (f_ && fwrite(d, 1, n, f_) != n) {
        fclose(f_);  // disk full: stop writing, Finish() reports nothing usable
        f_ = nullptr;
    }
    pos_ += n;
}

bool Mp4Writer::Open(const std::string& path, int rotation, int opusPreSkip) {
    std::lock_guard<std::mutex> l(m_);
    if (f_) return false;
    f_ = OpenFile(path, "wb");
    if (!f_) return false;
    path_ = path;
    rotation_ = ((rotation % 360) + 360) % 360;
    preSkip_ = opusPreSkip;
    pos_ = 0;
    video_.clear();
    audio_.clear();
    sps_.clear();
    pps_.clear();
    Box ftyp;
    ftyp.str("isom", 4);
    ftyp.u32(0x200);
    ftyp.str("isomiso2avc1mp41", 16);
    Box fb = ftyp.wrap("ftyp");
    Write(fb.b.data(), fb.b.size());
    mdatStart_ = pos_;
    Box mdat;
    mdat.u32(1);  // 64-bit size follows, filled in by Finish()
    mdat.str("mdat", 4);
    mdat.u64(0);
    Write(mdat.b.data(), mdat.b.size());
    return true;
}

void Mp4Writer::AddVideo(const uint8_t* p, size_t n, bool key, int64_t ptsUs, int width, int height) {
    std::lock_guard<std::mutex> l(m_);
    if (!f_) return;
    if (video_.empty() && !key) return;  // must start with a keyframe
    std::vector<uint8_t> out;
    out.reserve(n + 16);
    ForEachNal(p, n, [&](const uint8_t* nal, size_t len) {
        int type = nal[0] & 0x1f;
        if (type == 7) {
            if (sps_.empty()) sps_.assign(nal, nal + len);
            return;
        }
        if (type == 8) {
            if (pps_.empty()) pps_.assign(nal, nal + len);
            return;
        }
        if (type == 9) return;  // access unit delimiter
        uint8_t h[4] = {(uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len};
        out.insert(out.end(), h, h + 4);
        out.insert(out.end(), nal, nal + len);
    });
    if (out.empty() || sps_.size() < 4 || pps_.empty()) return;
    if (!video_.empty() && ptsUs <= video_.back().time) ptsUs = video_.back().time + 1;
    if (video_.empty()) width_ = width, height_ = height;
    video_.push_back({pos_, (uint32_t)out.size(), ptsUs, key});
    Write(out.data(), out.size());
}

void Mp4Writer::AddAudio(const uint8_t* d, size_t n) {
    std::lock_guard<std::mutex> l(m_);
    if (!f_ || n == 0) return;
    audio_.push_back({pos_, (uint32_t)n, 0, true});
    Write(d, n);
}

void Mp4Writer::Abort() {
    std::lock_guard<std::mutex> l(m_);
    if (!f_ && path_.empty()) return;
    if (f_) fclose(f_);
    f_ = nullptr;
    if (!path_.empty()) RemoveFile(path_);
    path_.clear();
}

double Mp4Writer::Finish() {
    std::lock_guard<std::mutex> l(m_);
    if (!f_ || video_.size() < 2) {
        if (f_) fclose(f_);
        f_ = nullptr;
        if (!path_.empty()) RemoveFile(path_);
        path_.clear();
        return 0;
    }
    // ---- video
    std::vector<uint32_t> vd, vs, keys;
    std::vector<uint64_t> vo;
    uint64_t vDur = 0;
    for (size_t i = 0; i < video_.size(); i++) {
        uint32_t d;
        if (i + 1 < video_.size()) d = (uint32_t)std::max<int64_t>(1, (video_[i + 1].time - video_[i].time) * kVideoScale / 1000000);
        else d = vd.empty() ? kVideoScale / 30 : vd.back();
        vd.push_back(d);
        vDur += d;
        vs.push_back(video_[i].size);
        vo.push_back(video_[i].offset);
        if (video_[i].key) keys.push_back((uint32_t)i + 1);
    }
    Box avcC;
    avcC.u8(1);
    avcC.u8(sps_[1]); avcC.u8(sps_[2]); avcC.u8(sps_[3]);
    avcC.u8(0xff);  // 4-byte NAL lengths
    avcC.u8(0xe1);
    avcC.u16((uint32_t)sps_.size());
    avcC.bytes(sps_);
    avcC.u8(1);
    avcC.u16((uint32_t)pps_.size());
    avcC.bytes(pps_);
    Box avc1;
    avc1.zeros(6);
    avc1.u16(1);
    avc1.zeros(16);
    avc1.u16((uint32_t)width_);
    avc1.u16((uint32_t)height_);
    avc1.u32(0x00480000); avc1.u32(0x00480000);
    avc1.u32(0);
    avc1.u16(1);
    avc1.zeros(32);
    avc1.u16(0x18);
    avc1.u16(0xffff);
    avc1.add(avcC.wrap("avcC"));
    Box vstsd;
    vstsd.u32(1);
    vstsd.add(avc1.wrap("avc1"));
    Box vstbl = Stbl(vstsd.full("stsd", 0), vd, &keys, vs, vo);

    // ---- audio
    std::vector<uint32_t> ad(audio_.size(), kOpusFrame), as;
    std::vector<uint64_t> ao;
    for (auto& s : audio_) { as.push_back(s.size); ao.push_back(s.offset); }
    uint64_t aDur = (uint64_t)audio_.size() * kOpusFrame;
    Box dOps;
    dOps.u8(0);
    dOps.u8(1);  // mono
    dOps.u16((uint32_t)preSkip_);
    dOps.u32(48000);
    dOps.u16(0);
    dOps.u8(0);
    Box opus;
    opus.zeros(6);
    opus.u16(1);
    opus.zeros(8);
    opus.u16(1);   // channels
    opus.u16(16);  // sample size
    opus.u32(0);
    opus.u32(kAudioScale << 16);
    opus.add(dOps.wrap("dOps"));
    Box astsd;
    astsd.u32(1);
    astsd.add(opus.wrap("Opus"));
    Box astbl = Stbl(astsd.full("stsd", 0), ad, nullptr, as, ao);

    uint64_t vMs = vDur * 1000 / kVideoScale, aMs = aDur * 1000 / kAudioScale, movieMs = std::max(vMs, aMs);
    Box mvhd;
    mvhd.u32(0); mvhd.u32(0);
    mvhd.u32(1000);
    mvhd.u32((uint32_t)movieMs);
    mvhd.u32(0x00010000);
    mvhd.u16(0x0100);
    mvhd.zeros(10);
    Matrix(mvhd, 0);
    mvhd.zeros(24);
    mvhd.u32(audio_.empty() ? 2 : 3);
    Box moov;
    moov.add(mvhd.full("mvhd", 0));
    moov.add(Trak(1, true, vMs, kVideoScale, vDur, rotation_, width_, height_, vstbl));
    if (!audio_.empty()) moov.add(Trak(2, false, aMs, kAudioScale, aDur, 0, 0, 0, astbl));
    Box mb = moov.wrap("moov");

    uint64_t mdatSize = pos_ - mdatStart_;
    Write(mb.b.data(), mb.b.size());
    bool ok = f_ != nullptr;
    if (ok) {
        uint8_t sz[8];
        for (int i = 0; i < 8; i++) sz[i] = (uint8_t)(mdatSize >> (56 - 8 * i));
        ok = FileSeek(f_, (int64_t)mdatStart_ + 8) && fwrite(sz, 1, 8, f_) == 8;
        ok = fclose(f_) == 0 && ok;
        f_ = nullptr;
    }
    if (!ok) {
        RemoveFile(path_);
        path_.clear();
        return 0;
    }
    path_.clear();
    return movieMs / 1000.0;
}

}  // namespace ls
