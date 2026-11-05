// LunaShare - group calls: voice (everyone hears everyone, mixed by the voice module) and video (one picture at a
// time: each person chooses whose video they watch, and only those who watch someone get that person's video).
//
// Two ways to carry it, chosen by whoever starts the call:
//  - directly: every device sends its packets to every other one over the normal end-to-end encrypted connections
//    (CallJoin tells the others "I'm in");
//  - through the server ("hub"): everyone sends each packet once to the server, which passes it to the people it is
//    addressed to; the packets are sealed with a key that only the people invited to the call have. Who is in the
//    call is the list of devices the server has in the hub. Easier on phones (one upload instead of one per person),
//    and group members who don't know each other never connect directly.
//
// Video packets ([2][x], see modules/video/video_core.cpp) are routed by kind: frames ([2][1]) only to the people
// watching me, camera state ([2][3]) and codecs ([2][5]) to everyone, and what a viewer says back about a picture
// (keyframe requests [2][2], size limits [2][4], receiver reports [2][6]) only to the person whose video it watches.
// The video module itself still sees one other side: it decodes the person being watched, and it hears the codecs
// everyone can decode (combined), so it picks one they all can.
#include <algorithm>
#include <cstring>

#include "crypto.h"
#include "engine.h"
#include "platform.h"

namespace ls {

static bool IsAudio(const uint8_t* d, size_t n) { return n > 0 && d[0] == 1; }
static int VideoKind(const uint8_t* d, size_t n) { return n >= 2 && d[0] == 2 ? d[1] : 0; }

// ---------------------------------------------------------------- starting, answering, leaving

uint64_t Engine::StartGroupCall(const std::string& key, const std::string& media) {
    const Group* g = GroupOf(key);
    if (!g || g->left || call.state != CallState::None) return 0;
    if (std::find(localCaps.begin(), localCaps.end(), CapForMedia(media)) == localCaps.end()) return 0;
    call = CallInfo();
    call.id = RandomU64();
    call.peer = key;
    call.group = key;
    call.outgoing = true;
    call.state = CallState::Active;  // I'm in; the others join when they answer
    call.started = call.answered = SteadyMs();
    call.aloneSince = SteadyMs();
    call.media = media == "video" ? "video" : "voice";
    call.hub = settings.groupsViaServer && wan_.Running() && wan_.Registered();
    if (call.hub) {
        call.hubId = RandomHex(16);
        call.key.resize(32);
        SecureRandom(&call.key[0], 32);
    }
    json inv = {{"call", call.id}, {"media", call.media}, {"gid", key.substr(1)}, {"ts", NowMs()}};
    if (call.hub) {
        inv["hub"] = call.hubId;
        inv["key"] = ToHex((const uint8_t*)call.key.data(), 32);
    }
    for (auto& d : DevicesOf(key)) {
        call.invited.push_back(d);
        if (!QueueToPeer(d, Msg::CallInvite, inv)) {
            MailTo(d, Msg::CallInvite, inv);  // (someone away gets it if they come back within a minute)
            if (!call.hub && !GroupOnlyHidden(d)) ConnectPeer(d);
        }
    }
    activeCallId_ = call.id;
    groupCall_ = true;
    SyncGroupMedia();
    if (call.hub) wan_.HubJoin(call.hubId);
    Emit({EventType::CallStarted, key, "Call started", call.id});
    return call.id;
}

void Engine::AcceptGroupCall() {
    call.state = CallState::Active;
    call.answered = SteadyMs();
    call.aloneSince = SteadyMs();
    activeCallId_ = call.id;
    groupCall_ = true;
    SyncGroupMedia();
    if (call.hub) {
        wan_.HubJoin(call.hubId);
    } else {
        json j = {{"call", call.id}, {"gid", call.group.substr(1)}};
        for (auto& d : DevicesOf(call.group))
            if (!QueueToPeer(d, Msg::CallJoin, j) && !GroupOnlyHidden(d)) ConnectPeer(d);  // (CallJoin again in OnReady)
    }
    Emit({EventType::CallStarted, call.group, "Call started", call.id});
}

void Engine::FinishGroupCall(const std::string& reason) {
    CallInfo c = call;
    json end = {{"call", c.id}, {"gid", c.group.substr(1)}, {"reason", reason}};
    if (c.state == CallState::IncomingRinging) {
        if (reason == "declined") QueueToPeer(c.inviter, Msg::CallEnd, end);
    } else {
        // leaving: the others drop me; if nobody else is in the call, the ones still ringing stop
        if (c.hub) wan_.HubJoin("");
        else
            for (auto& p : c.peers) QueueToPeer(p.dev, Msg::CallEnd, end);
        if (c.peers.empty()) {
            json stop = end;
            stop["reason"] = "ended";
            for (auto& d : c.invited) QueueToPeer(d, Msg::CallEnd, stop);
        }
    }
    activeCallId_ = 0;
    groupCall_ = false;
    {
        std::lock_guard<std::mutex> l(mediaMtx_);
        gc_.clear();
        gcSpot_.clear();
        gcKey_.clear();
        gcMyCam_.clear();
        gcMyCaps_.clear();
        gcDecSent_ = 0;
    }
    for (auto& cn : conns) cn->callPriority = false;
    call = CallInfo();
    std::string text;
    if (c.state == CallState::Active) {
        int secs = (int)((SteadyMs() - c.answered) / 1000);
        char b[64];
        snprintf(b, sizeof(b), "%d:%02d", secs / 60, secs % 60);
        text = std::string("Group call - ") + b;
    } else {
        text = reason == "declined" ? "Declined group call" : "Missed group call";
    }
    AddSystemMessage(c.group, text);
    Emit({EventType::CallEnded, c.group, text, c.id, reason});
}

// ---------------------------------------------------------------- signalling

void Engine::HandleGroupCall(const std::shared_ptr<Connection>& c, Msg type, const json& j) {
    std::string gid = j.value("gid", "");
    uint64_t id = j.value("call", (uint64_t)0);
    std::string key = "#" + gid;
    auto dv = devices.find(c->peerId);
    if (dv == devices.end() || dv->second.accountPub.empty()) return;
    const std::string& acc = dv->second.accountPub;
    const Group* g = GroupOf(key);
    if (!g || g->left || !g->Has(acc) || !id) return;
    switch (type) {
        case Msg::CallInvite: {
            if (acc == MyAccount()) return;
            if (NowMs() - j.value("ts", NowMs()) > 60000) return;  // (an old invitation from the mailbox)
            std::string media = j.value("media", "voice");
            bool supported = std::find(localCaps.begin(), localCaps.end(), CapForMedia(media)) != localCaps.end();
            if (call.id == id) return;
            if (!supported || call.state != CallState::None) {
                AddSystemMessage(key, supported ? "Missed group call (you were in another call)" : "Missed group call - calls are not installed on this device (voice module missing)");
                return;
            }
            call = CallInfo();
            call.id = id;
            call.peer = key;
            call.group = key;
            call.inviter = c->peerId;
            call.outgoing = false;
            call.state = CallState::IncomingRinging;
            call.started = SteadyMs();
            call.media = media;
            std::string hub = j.value("hub", ""), k = j.value("key", "");
            if (!hub.empty()) {
                call.key.resize(32);
                if (hub.size() != 32 || !FromHex(k, (uint8_t*)&call.key[0], 32)) {
                    call = CallInfo();
                    return;
                }
                call.hub = true;
                call.hubId = hub;
            }
            Emit({EventType::CallIncoming, key, "Incoming group call", id});
            break;
        }
        case Msg::CallJoin: {
            if (call.id != id || call.hub) return;
            if (call.state != CallState::Active) return;
            bool known = false;
            for (auto& p : call.peers) known |= p.dev == c->peerId;
            if (!known) GroupPeerJoined(c->peerId);
            if (!j.value("reply", false)) Queue(c, Msg::CallJoin, {{"call", id}, {"gid", gid}, {"reply", true}});
            break;
        }
        case Msg::CallEnd: {
            if (call.id != id) return;
            std::string reason = j.value("reason", "");
            if (call.state == CallState::IncomingRinging) {
                if (reason == "ended" || c->peerId == call.inviter) FinishGroupCall("missed");
            } else if (!call.hub) {
                GroupPeerLeft(c->peerId);
            }
            break;
        }
        case Msg::CallWant: {
            if (call.id != id || call.state != CallState::Active) return;
            bool want = j.value("want", false);
            std::string ask;
            int stream = 0;
            {
                std::lock_guard<std::mutex> l(mediaMtx_);
                for (auto& m : gc_)
                    if (m.dev == c->peerId) {
                        if (want && !m.wantsMyVideo) ask = m.dev;
                        m.wantsMyVideo = want;
                        stream = m.stream;
                    }
            }
            if (!ask.empty() && onMedia) {  // a new viewer: start them with a fresh keyframe
                const uint8_t key2[2] = {2, 2};
                onMedia(id, stream, key2, 2);
            }
            break;
        }
        default:
            break;
    }
}

// a hub message: ['M'][media] or ['C'][u8 type][json], sealed with the call key
void Engine::GroupHubIn(int from, const std::string& msg) {
    std::string key, dev;
    {
        std::lock_guard<std::mutex> l(mediaMtx_);
        if (!gcHub_ || gcKey_.size() != 32) return;
        key = gcKey_;
        for (auto& m : gc_)
            if (m.hubIdx == from) dev = m.dev;
    }
    if (dev.empty()) return;
    std::string plain;
    if (!KeyOpen((const uint8_t*)key.data(), (const uint8_t*)msg.data(), msg.size(), plain) || plain.empty()) return;
    if (plain[0] == 'M') {
        GroupMediaIn(dev, (const uint8_t*)plain.data() + 1, plain.size() - 1);
    } else if (plain[0] == 'C' && plain.size() > 2) {
        json j = json::parse(plain.substr(2), nullptr, false);
        if (!j.is_object()) return;
        auto lk = Lock();
        auto c = std::make_shared<Connection>();  // (who it's from, for HandleGroupCall)
        c->peerId = dev;
        c->ready = true;
        c->transport = Transport::Internet;
        j["gid"] = call.group.size() > 1 ? call.group.substr(1) : "";
        j["call"] = call.id;
        HandleGroupCall(c, (Msg)(uint8_t)plain[1], j);
    }
}

void Engine::GroupHubMembers(int you, const std::vector<std::string>& ids) {
    (void)you;
    auto lk = Lock();
    if (call.state != CallState::Active || !call.hub) return;
    const Group* g = GroupOf(call.group);
    std::vector<std::string> now;
    for (auto& id : ids) {
        if (id.empty() || id == settings.deviceId) continue;
        auto d = devices.find(id);
        if (d == devices.end() || !g || !g->Has(d->second.accountPub)) continue;  // (not someone of the group)
        now.push_back(id);
    }
    call.hubIds = ids;
    for (auto& p : call.peers) {  // the server's numbers for everyone
        p.hubIdx = -1;
        for (size_t i = 0; i < ids.size(); i++)
            if (ids[i] == p.dev) p.hubIdx = (int)i;
    }
    SyncGroupMedia();
    for (auto& p : std::vector<CallPeer>(call.peers))
        if (std::find(now.begin(), now.end(), p.dev) == now.end()) GroupPeerLeft(p.dev);
    for (auto& id : now) {
        bool known = false;
        for (auto& p : call.peers) known |= p.dev == id;
        if (!known) GroupPeerJoined(id);
    }
}

void Engine::GroupPeerJoined(const std::string& dev) {
    CallPeer p;
    p.dev = dev;
    auto d = devices.find(dev);
    p.acc = d != devices.end() ? d->second.accountPub : "";
    p.stream = call.nextStream++;
    for (size_t i = 0; i < call.hubIds.size(); i++)
        if (call.hubIds[i] == dev) p.hubIdx = (int)i;
    call.peers.push_back(p);
    call.aloneSince = 0;
    if (auto c = ReadyConn(dev)) c->callPriority = true;
    SyncGroupMedia();
    // the newcomer learns my camera state and codecs (the module only sends them when they change)
    std::string cam, caps;
    {
        std::lock_guard<std::mutex> l(mediaMtx_);
        cam = gcMyCam_;
        caps = gcMyCaps_;
    }
    for (auto* pkt : {&caps, &cam})
        if (!pkt->empty()) {
            if (call.hub) {
                std::string plain = "M" + *pkt;
                int idx = -1;
                {
                    std::lock_guard<std::mutex> l(mediaMtx_);
                    for (auto& m : gc_)
                        if (m.dev == dev) idx = m.hubIdx;
                }
                if (idx >= 0) wan_.HubSend(1u << idx, KeySeal((const uint8_t*)call.key.data(), plain));
            } else if (auto c = ReadyConn(dev)) {
                QueueMediaTo(c, call.id, (const uint8_t*)pkt->data(), pkt->size());
            }
        }
    UpdateSpotlight();
}

void Engine::GroupPeerLeft(const std::string& dev) {
    int stream = -1;
    for (auto it = call.peers.begin(); it != call.peers.end(); ++it)
        if (it->dev == dev) {
            stream = it->stream;
            call.peers.erase(it);
            break;
        }
    if (stream < 0) return;
    if (call.peers.empty()) call.aloneSince = SteadyMs();
    if (call.spotlight == dev) {
        call.spotlight.clear();
        call.spotlightPinned = false;
    }
    SyncGroupMedia();
    if (onMediaStreamEnd) onMediaStreamEnd(stream);
    UpdateSpotlight();
}

void Engine::GroupControl(const std::string& dev, Msg type, const json& j) {
    if (call.hub) {
        int idx = -1;
        {
            std::lock_guard<std::mutex> l(mediaMtx_);
            for (auto& m : gc_)
                if (m.dev == dev) idx = m.hubIdx;
        }
        if (idx < 0) return;
        std::string plain = "C";
        plain += (char)(uint8_t)type;
        plain += j.dump();
        wan_.HubSend(1u << idx, KeySeal((const uint8_t*)call.key.data(), plain));
    } else {
        QueueToPeer(dev, type, j);
    }
}

void Engine::SetSpotlight(const std::string& device) {
    auto lk = Lock();
    if (call.state != CallState::Active || call.group.empty()) return;
    call.spotlightPinned = !device.empty();
    if (!device.empty()) {
        std::string old = call.spotlight;
        call.spotlight = device;
        if (old != device) {
            if (!old.empty()) GroupControl(old, Msg::CallWant, {{"call", call.id}, {"gid", call.group.substr(1)}, {"want", false}});
            GroupControl(device, Msg::CallWant, {{"call", call.id}, {"gid", call.group.substr(1)}, {"want", true}});
            SyncGroupMedia();
        }
    }
    UpdateSpotlight();
}

// Automatic choice: keep watching someone whose camera is on; else the one who talked last with the camera on.
void Engine::UpdateSpotlight() {
    if (call.state != CallState::Active || call.group.empty() || call.media != "video") return;
    {
        std::lock_guard<std::mutex> l(mediaMtx_);
        for (auto& p : call.peers)
            for (auto& m : gc_)
                if (m.dev == p.dev) {
                    p.cameraOn = m.cameraOn;
                    p.talking = m.talking;
                }
    }
    if (call.spotlightPinned) return;
    const CallPeer* cur = nullptr;
    const CallPeer* best = nullptr;
    for (auto& p : call.peers) {
        if (p.dev == call.spotlight) cur = &p;
        if (p.cameraOn && (!best || p.talking > best->talking)) best = &p;
    }
    std::string want = cur && cur->cameraOn ? call.spotlight : best ? best->dev : (cur ? cur->dev : "");
    if (cur && cur->cameraOn && best && best->talking > cur->talking + 4000) want = best->dev;  // follow who speaks
    if (want.empty() && !call.peers.empty()) want = call.peers[0].dev;
    if (want == call.spotlight) {
        // (ask again now and then: a lost request, or the other side restarted its call)
        return;
    }
    std::string old = call.spotlight;
    call.spotlight = want;
    if (!old.empty()) GroupControl(old, Msg::CallWant, {{"call", call.id}, {"gid", call.group.substr(1)}, {"want", false}});
    if (!want.empty()) GroupControl(want, Msg::CallWant, {{"call", call.id}, {"gid", call.group.substr(1)}, {"want", true}});
    SyncGroupMedia();
}

void Engine::SyncGroupMedia() {
    int camStream = -1;
    uint8_t cam = 0;
    {
    std::lock_guard<std::mutex> l(mediaMtx_);
    std::vector<GcMember> next;
    for (auto& p : call.peers) {
        GcMember m;
        for (auto& o : gc_)
            if (o.dev == p.dev) m = o;  // keeps what the media threads learned (wants, codecs, camera, hub number)
        m.dev = p.dev;
        m.stream = p.stream;
        m.hubIdx = p.hubIdx;
        if (!call.hub) m.conn = ReadyConn(p.dev);
        next.push_back(m);
    }
    gc_ = next;
    bool spotChanged = gcSpot_ != call.spotlight;
    gcSpot_ = call.spotlight;
    gcHub_ = call.hub;
    gcKey_ = call.key;
    // the video module hears the camera state of whoever is watched now (it only learns it when it changes otherwise)
    if (spotChanged)
        for (auto& m : gc_)
            if (m.dev == gcSpot_) {
                camStream = m.stream;
                cam = m.cameraOn ? 1 : 0;
            }
    }
    if (camStream >= 0 && onMedia && activeCallId_.load()) {
        const uint8_t p[3] = {2, 3, cam};
        onMedia(activeCallId_.load(), camStream, p, 3);
    }
}

void Engine::GroupCallTick() {
    if (call.group.empty()) return;
    if (call.state == CallState::Active) {
        // alone for a while (nobody answered, or everyone left): the call ends
        if (call.peers.empty() && call.aloneSince && SteadyMs() - call.aloneSince > (call.answered == call.started && call.outgoing ? 60000 : 30000)) {
            FinishGroupCall("alone");
            return;
        }
        if (!call.hub) {  // connections that came back / changed
            std::lock_guard<std::mutex> l(mediaMtx_);
            for (auto& m : gc_)
                if (!m.conn.lock()) m.conn = ReadyConn(m.dev);
        }
        UpdateSpotlight();
        // keep asking for the picture I'm watching (once every few seconds is cheap; a lost request costs the video)
        static int64_t lastAsk = 0;
        if (!call.spotlight.empty() && SteadyMs() - lastAsk > 5000) {
            lastAsk = SteadyMs();
            GroupControl(call.spotlight, Msg::CallWant, {{"call", call.id}, {"gid", call.group.substr(1)}, {"want", true}});
        }
    }
}

// ---------------------------------------------------------------- media

bool Engine::SendGroupMedia(uint64_t id, const uint8_t* data, size_t len) {
    int vk = VideoKind(data, len);
    bool audio = IsAudio(data, len);
    std::vector<std::pair<std::shared_ptr<Connection>, int>> to;  // (connection, hub index)
    uint32_t mask = 0;
    bool hub;
    std::string key;
    {
        std::lock_guard<std::mutex> l(mediaMtx_);
        hub = gcHub_;
        key = gcKey_;
        if (vk == 3) gcMyCam_.assign((const char*)data, len);
        if (vk == 5) gcMyCaps_.assign((const char*)data, len);
        for (auto& m : gc_) {
            bool send = audio || vk == 3 || vk == 5 ? true
                      : vk == 1                     ? m.wantsMyVideo
                      : vk == 2 || vk == 4 || vk == 6 ? m.dev == gcSpot_
                                                    : false;
            if (!send) continue;
            if (hub) {
                if (m.hubIdx >= 0) mask |= 1u << m.hubIdx;
            } else if (auto c = m.conn.lock()) {
                to.emplace_back(c, -1);
            }
        }
    }
    if (hub) {
        if (!mask) return true;
        std::string plain(1, 'M');
        plain.append((const char*)data, len);
        return wan_.HubSend(mask, KeySeal((const uint8_t*)key.data(), plain));
    }
    bool ok = true;
    for (auto& [c, idx] : to) ok &= QueueMediaTo(c, id, data, len);
    return ok;
}

size_t Engine::GroupQueuedBytes() {
    std::vector<std::shared_ptr<Connection>> cs;
    {
        std::lock_guard<std::mutex> l(mediaMtx_);
        for (auto& m : gc_)
            if (m.wantsMyVideo)
                if (auto c = m.conn.lock()) cs.push_back(c);
    }
    size_t most = 0;
    for (auto& c : cs) {
        std::lock_guard<std::mutex> l(c->qm);
        size_t n = 0;
        for (auto& q : c->ctrl)
            if (q.first == Msg::Media) n += q.second.size();
        most = std::max(most, n);
    }
    return most;
}

void Engine::GroupMediaIn(const std::string& dev, const uint8_t* data, size_t len) {
    uint64_t id = activeCallId_.load();
    if (!id || !onMedia || len < 1) return;
    int stream = 0;
    bool spot = false, wants = false;
    uint8_t decAll = 0;
    bool decChanged = false;
    int vk = VideoKind(data, len);
    bool camChanged = false;
    {
        std::lock_guard<std::mutex> l(mediaMtx_);
        GcMember* m = nullptr;
        for (auto& x : gc_)
            if (x.dev == dev) m = &x;
        if (!m) return;
        stream = m->stream;
        spot = dev == gcSpot_;
        wants = m->wantsMyVideo;
        if (IsAudio(data, len)) m->talking = SteadyMs();
        if (vk == 3 && len >= 3 && m->cameraOn != (data[2] != 0)) {
            m->cameraOn = data[2] != 0;
            camChanged = true;
        }
        if (vk == 5 && len >= 4) {
            m->dec = data[2];
            unsigned all = 0xFF;
            for (auto& x : gc_)
                if (x.dec) all &= x.dec;
            decAll = (uint8_t)(all & 0xFF);
            if (decAll != gcDecSent_) {
                gcDecSent_ = decAll;
                decChanged = true;
            }
        }
    }
    if (IsAudio(data, len)) {
        onMedia(id, stream, data, len);
        return;
    }
    if (camChanged) {  // someone turned their camera on / off: maybe watch someone else now
        auto lk = Lock();
        UpdateSpotlight();
    }
    switch (vk) {
        case 1: if (spot) onMedia(id, stream, data, len); break;            // the picture I'm watching
        case 3: if (spot) onMedia(id, stream, data, len); break;            // its camera state
        case 2: case 4: case 6: if (wants) onMedia(id, stream, data, len); break;  // about my picture, from a viewer
        case 5:
            if (decChanged) {  // what everyone can decode
                uint8_t p[4] = {2, 5, decAll, len >= 4 ? data[3] : (uint8_t)0};
                onMedia(id, stream, p, sizeof(p));
            }
            break;
        default: break;
    }
}

}  // namespace ls
