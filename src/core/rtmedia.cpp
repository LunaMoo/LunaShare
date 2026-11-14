#include "rtmedia.h"

#include <algorithm>
#include <cstring>

#include "crypto.h"
#include "platform.h"

namespace ls {

namespace {
constexpr size_t kPiece = 1100;         // video bytes per datagram (all of it stays under ~1200 bytes on the wire)
constexpr int kFeedbackMs = 100;
constexpr int kMaxAsks = 3;             // a missing piece is asked for at most this often...
constexpr int kMissingMaxAge = 1000;    // ...and only while it's this fresh
constexpr int kKeepPieceMs = 1000;      // sent pieces kept for resending

void Put16(std::string& s, uint32_t v) { s.push_back((char)(v & 255)); s.push_back((char)((v >> 8) & 255)); }
void Put32(std::string& s, uint32_t v) { for (int i = 0; i < 4; i++) s.push_back((char)((v >> (8 * i)) & 255)); }
uint32_t Get16(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8; }
uint32_t Get32(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
}  // namespace

RtMedia::RtMedia(const uint8_t key[32], uint64_t callId, Out out, In in)
    : tag_(TagFor(callId)), out_(std::move(out)), in_(std::move(in)), ring_(kRing) {
    memcpy(key_, key, 32);
    int64_t now = SteadyMs();
    lastPace_ = lastIncrease_ = sentWindowAt_ = baseRotate_ = lossRotate_ = fbSince_ = rateSince_ = now;
    thread_ = std::thread([this] { Loop(); });
}

RtMedia::~RtMedia() {
    run_ = false;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    memset(key_, 0, sizeof(key_));
}

// ------------------------------------------------------------------------------------------------ sending

std::string RtMedia::Seal(uint8_t kind, const std::string& body, uint32_t* pseqOut) {
    uint32_t pseq = pseqOut_++;
    if (pseqOut) *pseqOut = pseq;
    std::string plain;
    plain.reserve(9 + body.size());
    plain.push_back((char)kind);
    Put32(plain, pseq);
    Put32(plain, (uint32_t)SteadyMs());
    plain += body;
    std::string d;
    d.reserve(4 + 40 + plain.size());
    Put32(d, tag_);
    d += KeySeal(key_, plain);
    return d;
}

void RtMedia::Emit(const std::string& datagram) {
    pendingOut_.push_back(datagram);
    sentBytesWindow_ += (int)datagram.size();
}

void RtMedia::SendNow(uint8_t kind, const std::string& body) { Emit(Seal(kind, body)); }

void RtMedia::FlushOut() {
    // one thread at a time, so datagrams leave in the order they were numbered (the other side counts gaps)
    std::lock_guard<std::mutex> ol(outM_);
    std::vector<std::string> out;
    {
        std::lock_guard<std::mutex> l(m_);
        out.swap(pendingOut_);
    }
    for (auto& d : out) out_((const uint8_t*)d.data(), d.size());
}

bool RtMedia::Send(const uint8_t* d, size_t n) {
    if (!run_ || !n || n > (8u << 20)) return false;
    {
        std::lock_guard<std::mutex> l(m_);
        if (!ready_) return false;
        int64_t now = SteadyMs();
        if (d[0] == 2 && n > 2 && d[1] == 1) {  // a video frame: in pieces, paced
            // more than ~300 ms already waiting: the path can't take this frame (unless nothing waits at all - a big
            // keyframe must get through some time)
            size_t limit = std::max<size_t>(24000, (size_t)estimate_.load() * 3 / 80);
            if (pacedBytes_ > limit) return false;
            uint32_t frame = frameOut_++;
            size_t count = (n + kPiece - 1) / kPiece;
            if (count > 65535) return false;
            for (size_t i = 0; i < count; i++) {
                size_t off = i * kPiece, len = std::min(kPiece, n - off);
                Outgoing o;
                o.kind = 2;
                o.frame = frame;
                o.idx = (uint16_t)i;
                o.count = (uint16_t)count;
                o.bytes.assign((const char*)d + off, len);
                o.queuedAt = now;
                pacedBytes_ += len + 60;
                paced_.push_back(std::move(o));
            }
        } else {  // voice and the video module's small messages: straight out
            SendNow(d[0] == 2 ? 3 : 1, std::string((const char*)d, n));
        }
    }
    cv_.notify_one();
    FlushOut();
    return true;
}

size_t RtMedia::Queued() {
    std::lock_guard<std::mutex> l(m_);
    return pacedBytes_;
}

void RtMedia::Pace(int64_t now) {
    double rate = std::max(estimate_.load() * 1.25, 150000.0) / 8.0;  // bytes per second
    double dt = (double)(now - lastPace_) / 1000.0;
    lastPace_ = now;
    budget_ = std::min(budget_ + rate * dt, std::max(6000.0, rate * 0.02));
    while (!paced_.empty()) {
        Outgoing& o = paced_.front();
        size_t size = o.bytes.size() + 60;
        if (now - o.queuedAt > 1000) {  // (stale: it couldn't be shown any more)
            pacedBytes_ -= std::min(pacedBytes_, size);
            paced_.pop_front();
            continue;
        }
        if (budget_ < (double)size) break;
        budget_ -= (double)size;
        std::string body;
        body.reserve(o.bytes.size() + 12);
        if (o.kind == 8) Put32(body, o.orig);
        Put32(body, o.frame);
        Put16(body, o.idx);
        Put16(body, o.count);
        body += o.bytes;
        uint32_t pseq = 0;
        Emit(Seal(o.kind, body, &pseq));
        if (o.kind == 2) {  // kept for resending (a resend is found by its original number)
            Piece& p = ring_[pseq % kRing];
            p.pseq = pseq;
            p.frame = o.frame;
            p.idx = o.idx;
            p.count = o.count;
            p.sentAt = now;
            p.resentAt = 0;
            p.bytes = std::move(o.bytes);
        } else {
            Piece& p = ring_[o.orig % kRing];
            if (p.pseq == o.orig) p.resentAt = now;
        }
        pacedBytes_ -= std::min(pacedBytes_, size);
        paced_.pop_front();
    }
}

// ------------------------------------------------------------------------------------------------ receiving

void RtMedia::Input(const uint8_t* d, size_t n) {
    if (!run_ || n < 4 + 40 + 9 || Get32(d) != tag_) return;
    if (int pct = testDropPct.load()) {
        dropRng_ = dropRng_ * 1103515245u + 12345u;
        if ((int)((dropRng_ >> 16) % 100) < pct) return;
    }
    std::string plain;
    if (!KeyOpen(key_, d + 4, n - 4, plain) || plain.size() < 9) return;
    const uint8_t* p = (const uint8_t*)plain.data();
    uint8_t kind = p[0];
    uint32_t pseq = Get32(p + 1), ts = Get32(p + 5);
    const uint8_t* b = p + 9;
    size_t bn = plain.size() - 9;
    std::vector<std::string> deliver;
    {
        std::lock_guard<std::mutex> l(m_);
        int64_t now = SteadyMs();
        if (seen_.count(pseq)) return;  // (a duplicate, or replayed)
        seen_.insert(pseq);
        seenOrder_.push_back(pseq);
        if (seenOrder_.size() > 4096) {
            seen_.erase(seenOrder_.front());
            seenOrder_.pop_front();
        }
        heardPeer_ = true;
        lastHeard_ = now;
        // losses: gaps in the numbering
        if (!haveHigh_) {
            haveHigh_ = true;
            highIn_ = windowStartHigh_ = pseq;
        } else if ((int32_t)(pseq - highIn_) > 0) {
            uint32_t gap = pseq - highIn_ - 1;
            for (uint32_t s = highIn_ + 1, k = 0; s != pseq && k < 300; s++, k++) missing_[s] = Missing{now, 0, 0};
            if (gap && now - lastFeedback_ >= 20) wantFeedback_ = true;  // ask for the lost pieces right away
            highIn_ = pseq;
        } else {
            missing_.erase(pseq);  // (arrived out of order)
        }
        gotWindow_++;
        bytesWindow_ += (int)n;
        // delay above the lowest seen lately: how long it waited in queues on the way (clock offsets cancel out)
        int32_t dly = (int32_t)((uint32_t)now - ts);
        if (now - baseRotate_ > 5000) {
            baseRotate_ = now;
            basePrev_ = baseCur_;
            baseCur_ = dly;
        }
        baseCur_ = std::min(baseCur_, dly);
        int32_t base = std::min(basePrev_, baseCur_);
        qdelaySum_ += std::max(0, dly - base);
        qdelayCount_++;
        switch (kind) {
            case 1:
            case 3:
                deliver.emplace_back((const char*)b, bn);
                break;
            case 2:
                if (bn > 8) TakePiece(Get32(b), (uint16_t)Get16(b + 4), (uint16_t)Get16(b + 6), b + 8, bn - 8, now);
                break;
            case 8:
                if (bn > 12) {
                    missing_.erase(Get32(b));
                    TakePiece(Get32(b + 4), (uint16_t)Get16(b + 8), (uint16_t)Get16(b + 10), b + 12, bn - 12, now);
                }
                break;
            case 4:
                OnFeedback(b, bn, now);
                break;
            case 5:
                if (bn >= 1 && b[0] == 1) lastConfirm_ = now;
                break;
            case 6:
                if (bn >= 4) SendNow(7, std::string((const char*)b, 4));
                break;
            case 7:
                if (bn >= 4) {
                    int r = (int)((uint32_t)now - Get32(b));
                    if (r >= 0 && r < 10000) rtt_ = rtt_ ? (rtt_ * 7 + r) / 8 : r;
                }
                break;
            default:
                break;
        }
        FlushFrames(now, deliver);
        if (wantFeedback_) SendFeedback(now);
        ready_ = lastConfirm_ && now - lastConfirm_ < 3000;
    }
    FlushOut();
    for (auto& m : deliver) in_((const uint8_t*)m.data(), m.size());
}

void RtMedia::TakePiece(uint32_t frame, uint16_t idx, uint16_t count, const uint8_t* p, size_t n, int64_t now) {
    if (!count || idx >= count || !n) return;
    if (haveNextFrame_ && (int32_t)(frame - nextFrame_) < 0) return;  // (that frame was given up on already)
    if (!haveNextFrame_) {
        haveNextFrame_ = true;
        nextFrame_ = frame;
    }
    Frame& f = frames_[frame];
    if (!f.count) {
        f.count = count;
        f.parts.resize(count);
        f.firstAt = now;
    }
    if (f.count != count || !f.parts[idx].empty()) return;
    f.parts[idx].assign((const char*)p, n);
    f.got++;
    while (frames_.size() > 120) frames_.erase(frames_.begin());  // (something is badly wrong: don't grow)
}

int RtMedia::FrameDeadline() const { return std::clamp(rtt_ * 2 + 60, 120, 700); }  // (time for a piece to be asked for twice)

void RtMedia::FlushFrames(int64_t now, std::vector<std::string>& deliver) {
    if (!haveNextFrame_) return;
    for (;;) {
        auto it = frames_.find(nextFrame_);
        if (it != frames_.end() && it->second.got == it->second.count) {
            std::string whole;
            for (auto& part : it->second.parts) whole += part;
            deliver.push_back(std::move(whole));
            frames_.erase(it);
            nextFrame_++;
            blockedSince_ = 0;
            continue;
        }
        // not complete: wait for its missing pieces while they can still come, then go on without it
        auto later = frames_.upper_bound(nextFrame_);
        if (later == frames_.end()) break;
        if (!blockedSince_) blockedSince_ = it != frames_.end() ? std::min(it->second.firstAt, later->second.firstAt) : later->second.firstAt;
        if (now - blockedSince_ < FrameDeadline()) break;
        late_++;
        if (it != frames_.end()) frames_.erase(it);
        nextFrame_++;
        blockedSince_ = 0;
    }
}

void RtMedia::SendFeedback(int64_t now) {
    wantFeedback_ = false;
    lastFeedback_ = now;
    uint32_t expected = highIn_ - windowStartHigh_;
    windowStartHigh_ = highIn_;
    int got = gotWindow_;
    gotWindow_ = 0;
    // the rate arriving here, over half-second stretches (a feedback window can be a few milliseconds long)
    rateBytes_ += bytesWindow_;
    bytesWindow_ = 0;
    if (now - rateSince_ >= 500) {
        rateKbps_ = (int)((int64_t)rateBytes_ * 8 / (now - rateSince_));
        rateBytes_ = 0;
        rateSince_ = now;
    }
    int kbps = rateKbps_;
    int qd = qdelayCount_ ? (int)(qdelaySum_ / qdelayCount_) : 0;
    qdelaySum_ = 0;
    qdelayCount_ = 0;
    // what arrives here, lost: counted over the last 2-4 s (feedback windows are uneven - a gap sends one at once)
    lostCur_ += (int)expected - got;  // (signed: one that arrives late makes up for being counted lost before)
    expCur_ += (int)expected;
    if (now - lossRotate_ >= 2000) {
        lossRotate_ = now;
        lostPrev_ = lostCur_;
        expPrev_ = expCur_;
        lostCur_ = expCur_ = 0;
    }
    if (expPrev_ + expCur_ > 0) lossInPct10_ = std::max(0, (int)((int64_t)(lostPrev_ + lostCur_) * 1000 / (expPrev_ + expCur_)));
    // the missing pieces worth asking for again (each at most a few times, a round trip apart)
    std::vector<uint32_t> ask;
    int gapMs = std::max(rtt_ * 6 / 5, 40);
    for (auto it = missing_.begin(); it != missing_.end();) {
        Missing& m = it->second;
        if (now - m.since > kMissingMaxAge || (m.asks >= kMaxAsks && now - m.lastAsk > gapMs)) {
            it = missing_.erase(it);
            continue;
        }
        if (m.asks < kMaxAsks && (m.asks == 0 || now - m.lastAsk >= gapMs) && ask.size() < 60) {
            ask.push_back(it->first);
            m.asks++;
            m.lastAsk = now;
        }
        ++it;
    }
    std::string body;
    Put32(body, highIn_);
    Put16(body, (uint32_t)std::min(got, 65535));
    Put16(body, std::min<uint32_t>(expected, 65535));
    Put16(body, (uint32_t)std::clamp(qd, 0, 65535));
    Put16(body, (uint32_t)std::clamp(kbps, 0, 65535));
    body.push_back((char)ask.size());
    for (uint32_t s : ask) Put32(body, s);
    SendNow(4, body);
}

void RtMedia::OnFeedback(const uint8_t* b, size_t n, int64_t now) {
    if (n < 13) return;
    lastConfirm_ = now;  // (they hear me)
    int got = (int)Get16(b + 4), expected = (int)Get16(b + 6), qd = (int)Get16(b + 8), kbps = (int)Get16(b + 10);
    size_t nAsk = b[12];
    if (n < 13 + nAsk * 4) return;
    // loss: judged over at least ~half a second of their reports (one lost datagram in a small window isn't 50%)
    fbGot_ += got;
    fbExp_ += expected;
    double loss = -1;  // (not judged this time)
    if (fbExp_ >= 100 || (fbExp_ >= 20 && now - fbSince_ >= 1000)) {
        loss = std::clamp((double)(fbExp_ - fbGot_) / fbExp_, 0.0, 1.0);
        lossOutPct10_ = (lossOutPct10_ + (int)(loss * 1000)) / 2;
        fbGot_ = fbExp_ = 0;
        fbSince_ = now;
    }
    peerQueueMs_ = qd;
    // the estimate: down at once when a queue builds or much gets lost, up slowly while the path stays clear
    int est = estimate_.load();
    double dt = std::clamp((double)(now - lastIncrease_) / 1000.0, 0.0, 0.5);
    lastIncrease_ = now;
    // heavy loss while nothing queues up looks like a bad radio link, not a full one: sending less wouldn't help
    // there (the picture would only get worse), so the estimate just stops climbing
    bool randomLoss = loss > 0.10 && loss < 0.35 && qd < 30;
    if (randomLoss) {
        holdUntil_ = std::max(holdUntil_, now + 1000);
    } else if (qd > 60 || loss > 0.10) {
        if (now - lastDecrease_ > std::max(rtt_, 200)) {
            int recv = kbps * 1000;
            int base = recv > 0 ? std::min(est, recv) : est;
            est = std::max(kMinBps, (int)(base * 0.85));
            lastDecrease_ = now;
            holdUntil_ = now + 1500;
        }
    } else if (qd < 25 && loss < 0.03 && lossOutPct10_ < 30 && now >= holdUntil_) {
        double grow = est < 1000000 ? 0.15 : 0.08;  // per second: quicker while still low
        est = (int)(est * (1.0 + grow * dt));
        // not far above what is actually sent (an encoder sending less mustn't let the estimate run away)
        est = std::min(est, std::max(kStartBps, sentBps_ * 2 + 100000));
    }
    estimate_ = std::clamp(est, kMinBps, 50000000);
    // pieces they're missing: again, ahead of new ones, if still worth it
    int resends = 0;
    std::deque<Outgoing> front;
    for (size_t i = 0; i < nAsk; i++) {
        uint32_t s = Get32(b + 13 + i * 4);
        Piece& p = ring_[s % kRing];
        if (p.pseq != s || p.bytes.empty() || now - p.sentAt > kKeepPieceMs) continue;
        if (p.resentAt && now - p.resentAt < std::max(rtt_, 30)) continue;
        if (++resends > 64) break;
        Outgoing o;
        o.kind = 8;
        o.orig = s;
        o.frame = p.frame;
        o.idx = p.idx;
        o.count = p.count;
        o.bytes = p.bytes;
        o.queuedAt = now;
        pacedBytes_ += o.bytes.size() + 60;
        front.push_back(std::move(o));
        resent_++;
    }
    for (auto it = front.rbegin(); it != front.rend(); ++it) paced_.push_front(std::move(*it));
}

// ------------------------------------------------------------------------------------------------ timer

void RtMedia::Loop() {
    while (run_) {
        std::vector<std::string> deliver;
        {
            std::unique_lock<std::mutex> l(m_);
            cv_.wait_for(l, std::chrono::milliseconds(5));
            if (!run_) break;
            int64_t now = SteadyMs();
            Pace(now);
            if (heardPeer_ && now - lastFeedback_ >= kFeedbackMs && (gotWindow_ > 0 || !missing_.empty() || wantFeedback_))
                SendFeedback(now);
            // probes: until both sides hear each other, then now and then (keeps NAT mappings open, re-confirms)
            bool ready = lastConfirm_ && now - lastConfirm_ < 3000;
            if (now - lastProbe_ >= (ready ? 1000 : 150)) {
                lastProbe_ = now;
                SendNow(5, std::string(1, heardPeer_ && now - lastHeard_ < 3000 ? (char)1 : (char)0));
            }
            if (heardPeer_ && now - lastPing_ >= 500) {
                lastPing_ = now;
                std::string t;
                Put32(t, (uint32_t)now);
                SendNow(6, t);
            }
            FlushFrames(now, deliver);
            if (now - sentWindowAt_ >= 1000) {
                sentBps_ = (int)((int64_t)sentBytesWindow_ * 8 * 1000 / (now - sentWindowAt_));
                sentBytesWindow_ = 0;
                sentWindowAt_ = now;
            }
            ready_ = ready;
        }
        FlushOut();
        for (auto& m : deliver) in_((const uint8_t*)m.data(), m.size());
    }
}

RtStats RtMedia::Stats() {
    std::lock_guard<std::mutex> l(m_);
    RtStats s;
    s.ready = ready_;
    s.lossInPct10 = lossInPct10_;
    s.lossOutPct10 = lossOutPct10_;
    s.lateFrames = late_;
    s.resent = resent_;
    s.rttMs = rtt_;
    s.estimateKbps = estimate_.load() / 1000;
    s.queueDelayMs = peerQueueMs_;
    return s;
}

}  // namespace ls
