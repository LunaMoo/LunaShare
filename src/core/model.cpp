#include "model.h"
#include <algorithm>

#include "crypto.h"
#include "platform.h"

namespace ls {

std::string KnownDevice::Fingerprint() const {
    if (!pubKey.empty()) return KeyFingerprint(pubKey);
    std::string f;
    for (char c : id) {
        if (c == '-') continue;
        f += (char)toupper((unsigned char)c);
        if (f.size() == 4 || f.size() == 9) f += '-';
        if (f.size() >= 14) break;
    }
    return f;
}

const char* TStateName(TState s) {
    switch (s) {
        case TState::Hashing: return "Hashing";
        case TState::Offered: return "Waiting for accept";
        case TState::Pending: return "Incoming";
        case TState::Queued: return "Queued";
        case TState::Active: return "Transferring";
        case TState::Paused: return "Paused";
        case TState::Interrupted: return "Waiting for device";
        case TState::Verifying: return "Verifying";
        case TState::Completed: return "Done";
        case TState::Failed: return "Failed";
        case TState::Rejected: return "Declined";
        case TState::Cancelled: return "Cancelled";
    }
    return "?";
}

bool TStateFinal(TState s) {
    return s == TState::Completed || s == TState::Failed || s == TState::Rejected || s == TState::Cancelled;
}

template <class T>
static void Get(const json& j, const char* k, T& v) {
    auto it = j.find(k);
    if (it != j.end() && !it->is_null()) {
        try { v = it->get<T>(); } catch (...) {}
    }
}

json ToJson(const Settings& s, bool withIdentity) {
    json j = {
        {"displayName", s.displayName}, {"downloadDir", s.downloadDir}, {"tcpPort", s.tcpPort},
        {"discovery", s.discovery}, {"mdns", s.mdns}, {"broadcast", s.broadcast}, {"autoConnect", s.autoConnect}, {"autoAcceptTrusted", s.autoAcceptTrusted},
        {"autoAcceptAll", s.autoAcceptAll}, {"autoResume", s.autoResume}, {"bluetooth", s.bluetooth},
        {"btChannel", s.btChannel}, {"wanEnabled", s.wanEnabled}, {"wanServer", s.wanServer}, {"wanRoom", s.wanRoom},
        {"wanPassword", s.wanPassword}, {"wanNatPmp", s.wanNatPmp}, {"wanLocalPort", s.wanLocalPort},
        {"wanForwardedPort", s.wanForwardedPort}, {"speedLimitKBs", s.speedLimitKBs}, {"chunkKB", s.chunkKB},
        {"maxParallel", s.maxParallel}, {"notifySound", s.notifySound}, {"keepHistory", s.keepHistory},
        {"layout", (int)s.layout}, {"uiScale", s.uiScale}, {"theme", s.theme}, {"lastBrowseDir", s.lastBrowseDir},
        {"browserGrid", s.browserGrid}, {"emojiRecent", s.emojiRecent}, {"emojiTone", s.emojiTone}, {"browserPicturesOnly", s.browserPicturesOnly}, {"browserSort", s.browserSort},
        {"browserTile", s.browserTile}, {"callEchoCancel", s.callEchoCancel}, {"callNoiseSuppress", s.callNoiseSuppress},
        {"callAutoGain", s.callAutoGain}, {"callBitrate", s.callBitrate}, {"callMicBoost", s.callMicBoost}, {"callVideoRes", s.callVideoRes}, {"callVideoFps", s.callVideoFps}, {"callVideoAdaptive", s.callVideoAdaptive}, {"callCamera", s.callCamera}, {"virtualCamera", s.virtualCamera}, {"callVideoH264Only", s.callVideoH264Only}, {"callInputDevice", s.callInputDevice},
        {"callOutputDevice", s.callOutputDevice},
    };
    if (withIdentity) {
        j["deviceId"] = s.deviceId;
        j["identitySecret"] = s.identitySecret;
    }
    return j;
}

void FromJson(const json& j, Settings& s, bool withIdentity) {
    if (!j.is_object()) return;
    if (withIdentity) {
        Get(j, "deviceId", s.deviceId);
        Get(j, "identitySecret", s.identitySecret);
    }
    Get(j, "displayName", s.displayName);
    Get(j, "downloadDir", s.downloadDir);
    Get(j, "tcpPort", s.tcpPort);
    Get(j, "discovery", s.discovery);
    Get(j, "mdns", s.mdns);
    Get(j, "broadcast", s.broadcast);
    Get(j, "autoConnect", s.autoConnect);
    Get(j, "autoAcceptTrusted", s.autoAcceptTrusted);
    Get(j, "autoAcceptAll", s.autoAcceptAll);
    Get(j, "autoResume", s.autoResume);
    Get(j, "bluetooth", s.bluetooth);
    Get(j, "btChannel", s.btChannel);
    Get(j, "wanEnabled", s.wanEnabled);
    Get(j, "wanServer", s.wanServer);
    Get(j, "wanRoom", s.wanRoom);
    Get(j, "wanPassword", s.wanPassword);
    Get(j, "wanNatPmp", s.wanNatPmp);
    Get(j, "wanLocalPort", s.wanLocalPort);
    Get(j, "wanForwardedPort", s.wanForwardedPort);
    Get(j, "speedLimitKBs", s.speedLimitKBs);
    Get(j, "chunkKB", s.chunkKB);
    Get(j, "maxParallel", s.maxParallel);
    Get(j, "notifySound", s.notifySound);
    Get(j, "keepHistory", s.keepHistory);
    int layout = (int)s.layout;
    Get(j, "layout", layout);
    s.layout = (UiLayout)(layout < 0 || layout > 2 ? 0 : layout);
    Get(j, "uiScale", s.uiScale);
    Get(j, "theme", s.theme);
    Get(j, "lastBrowseDir", s.lastBrowseDir);
    Get(j, "browserGrid", s.browserGrid);
    Get(j, "emojiRecent", s.emojiRecent);
    Get(j, "emojiTone", s.emojiTone);
    if (s.emojiTone < 0 || s.emojiTone > 5) s.emojiTone = 0;
    Get(j, "browserPicturesOnly", s.browserPicturesOnly);
    Get(j, "browserSort", s.browserSort);
    Get(j, "browserTile", s.browserTile);
    Get(j, "callEchoCancel", s.callEchoCancel);
    Get(j, "callNoiseSuppress", s.callNoiseSuppress);
    Get(j, "callAutoGain", s.callAutoGain);
    Get(j, "callBitrate", s.callBitrate);
    Get(j, "callMicBoost", s.callMicBoost);
    Get(j, "callVideoRes", s.callVideoRes);
    Get(j, "callVideoFps", s.callVideoFps);
    Get(j, "callVideoAdaptive", s.callVideoAdaptive);
    Get(j, "callCamera", s.callCamera);
    Get(j, "virtualCamera", s.virtualCamera);
    Get(j, "callVideoH264Only", s.callVideoH264Only);
    s.callVideoRes = std::clamp(s.callVideoRes, 0, 6);
    s.callVideoFps = std::clamp(s.callVideoFps, 10, 120);
    if (s.callMicBoost < 0 || s.callMicBoost > 30) s.callMicBoost = 0;
    Get(j, "callInputDevice", s.callInputDevice);
    Get(j, "callOutputDevice", s.callOutputDevice);
    if (s.callBitrate < 8000 || s.callBitrate > 64000) s.callBitrate = 24000;
    if (s.browserSort < 0 || s.browserSort > 2) s.browserSort = 0;
    if (s.browserTile < 64 || s.browserTile > 400) s.browserTile = 130;
    if (s.chunkKB < 16) s.chunkKB = 16;
    if (s.chunkKB > 4096) s.chunkKB = 4096;
    if (s.maxParallel < 1) s.maxParallel = 1;
    if (s.maxParallel > 8) s.maxParallel = 8;
}

json ToJson(const KnownDevice& d) {
    return {{"id", d.id}, {"name", d.name}, {"customName", d.customName}, {"user", d.user}, {"host", d.host},
            {"platform", d.platform}, {"macs", d.macs}, {"lastIp", d.lastIp}, {"lastPort", d.lastPort},
            {"btAddr", d.btAddr}, {"firstSeen", d.firstSeen}, {"lastSeen", d.lastSeen}, {"trusted", d.trusted},
            {"blocked", d.blocked}, {"notes", d.notes}, {"pubKey", d.pubKey}, {"verified", d.verified},
            {"pendingKey", d.pendingKey}};
}

KnownDevice DeviceFromJson(const json& j) {
    KnownDevice d;
    Get(j, "id", d.id);
    Get(j, "name", d.name);
    Get(j, "customName", d.customName);
    Get(j, "user", d.user);
    Get(j, "host", d.host);
    Get(j, "platform", d.platform);
    Get(j, "macs", d.macs);
    Get(j, "lastIp", d.lastIp);
    Get(j, "lastPort", d.lastPort);
    Get(j, "btAddr", d.btAddr);
    Get(j, "firstSeen", d.firstSeen);
    Get(j, "lastSeen", d.lastSeen);
    Get(j, "trusted", d.trusted);
    Get(j, "blocked", d.blocked);
    Get(j, "notes", d.notes);
    Get(j, "pubKey", d.pubKey);
    Get(j, "verified", d.verified);
    Get(j, "pendingKey", d.pendingKey);
    return d;
}

json ToJson(const ChatMessage& m) {
    json j = {{"mid", m.mid}, {"out", m.outgoing}, {"ts", m.ts}, {"kind", (int)m.kind}, {"text", m.text},
              {"tids", m.tids}, {"batch", m.batch}, {"delivered", m.delivered}};
    if (!m.myReaction.empty() || !m.reactionSent) j["myReact"] = m.myReaction;
    if (!m.reactionSent) j["reactPending"] = true;
    if (!m.theirReaction.empty()) j["theirReact"] = m.theirReaction;
    return j;
}

ChatMessage MessageFromJson(const json& j) {
    ChatMessage m;
    Get(j, "mid", m.mid);
    Get(j, "out", m.outgoing);
    Get(j, "ts", m.ts);
    int k = 0;
    Get(j, "kind", k);
    m.kind = (MsgKind)k;
    Get(j, "text", m.text);
    Get(j, "tids", m.tids);
    Get(j, "batch", m.batch);
    Get(j, "delivered", m.delivered);
    Get(j, "myReact", m.myReaction);
    Get(j, "theirReact", m.theirReaction);
    bool pending = false;
    Get(j, "reactPending", pending);
    m.reactionSent = !pending;
    return m;
}

json ToJson(const Transfer& t) {
    return {{"tid", t.tid}, {"batch", t.batch}, {"batchName", t.batchName}, {"batchCount", t.batchCount},
            {"peer", t.peer}, {"out", t.outgoing}, {"name", t.name}, {"rel", t.rel}, {"localPath", t.localPath},
            {"partPath", t.partPath}, {"size", t.size}, {"mtime", t.mtime}, {"sha256", t.sha256},
            {"recvSha", t.recvSha}, {"state", (int)t.state}, {"pausedByPeer", t.pausedByPeer},
            {"error", t.error}, {"created", t.created}, {"finished", t.finished}, {"done", t.done.load()}};
}

void TransferFromJson(const json& j, Transfer& t) {
    Get(j, "tid", t.tid);
    Get(j, "batch", t.batch);
    Get(j, "batchName", t.batchName);
    Get(j, "batchCount", t.batchCount);
    Get(j, "peer", t.peer);
    Get(j, "out", t.outgoing);
    Get(j, "name", t.name);
    Get(j, "rel", t.rel);
    Get(j, "localPath", t.localPath);
    Get(j, "partPath", t.partPath);
    Get(j, "size", t.size);
    Get(j, "mtime", t.mtime);
    Get(j, "sha256", t.sha256);
    Get(j, "recvSha", t.recvSha);
    int st = 0;
    Get(j, "state", st);
    t.state = (TState)st;
    Get(j, "pausedByPeer", t.pausedByPeer);
    Get(j, "error", t.error);
    Get(j, "created", t.created);
    Get(j, "finished", t.finished);
    int64_t done = 0;
    Get(j, "done", done);
    t.done = done;
}

json ToJson(const HistoryEntry& h) {
    return {{"tid", h.tid}, {"peer", h.peer}, {"peerName", h.peerName}, {"out", h.outgoing}, {"name", h.name},
            {"path", h.path}, {"size", h.size}, {"sha256", h.sha256}, {"verified", h.verified}, {"ts", h.ts}};
}

HistoryEntry HistoryFromJson(const json& j) {
    HistoryEntry h;
    Get(j, "tid", h.tid);
    Get(j, "peer", h.peer);
    Get(j, "peerName", h.peerName);
    Get(j, "out", h.outgoing);
    Get(j, "name", h.name);
    Get(j, "path", h.path);
    Get(j, "size", h.size);
    Get(j, "sha256", h.sha256);
    Get(j, "verified", h.verified);
    Get(j, "ts", h.ts);
    return h;
}

bool SaveJsonFile(const std::string& path, const json& j) {
    std::string tmp = path + ".tmp";
    FILE* f = OpenFile(tmp, "wb");
    if (!f) return false;
    std::string s = j.dump(1, '\t', false, json::error_handler_t::replace);
    bool ok = fwrite(s.data(), 1, s.size(), f) == s.size();
    ok = (fclose(f) == 0) && ok;
    if (!ok) { RemoveFile(tmp); return false; }
    if (RenameFile(tmp, path)) return true;
    RemoveFile(path);  // some runtimes refuse to overwrite on Windows
    return RenameFile(tmp, path);
}

bool LoadJsonFile(const std::string& path, json& j) {
    FILE* f = OpenFile(path, "rb");
    if (!f) return false;
    std::string s;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    j = json::parse(s, nullptr, false);
    return !j.is_discarded();
}

}  // namespace ls
