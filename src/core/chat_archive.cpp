// LunaShare - chat archives: a conversation saved (compressed) to a file of its own, optionally cleared afterwards
// (keeping yesterday's and today's messages), listed, restored - and a contact asked to send our history back when
// both the chat and its archive are gone. Files are never copied: messages keep their links to them, so a file
// that's still on the disk opens from a restored chat like before.
#include <algorithm>
#include <ctime>
#include <set>

#include "engine.h"
#include "platform.h"

namespace ls {

static const char* kArchMagic = "LSARCH1";

static std::string ArchDir(Engine& e) { return JoinPath(e.ConfigPath(), "archives"); }

// the start of yesterday, local time (wall ms)
static int64_t StartOfYesterday() {
    time_t now = time(nullptr);
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &now);
#else
    localtime_r(&now, &lt);
#endif
    lt.tm_hour = lt.tm_min = lt.tm_sec = 0;
    lt.tm_mday -= 1;
    lt.tm_isdst = -1;
    return (int64_t)mktime(&lt) * 1000;
}

static std::string Stamp(int64_t ms) {
    time_t t = (time_t)(ms / 1000);
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    char b[32];
    strftime(b, sizeof b, "%Y%m%d-%H%M%S", &lt);
    return b;
}

// the files of these messages (their transfer records: names, sizes, hashes, where they are)
json Engine::HistoryFiles(const std::vector<ChatMessage>& msgs) {
    json files = json::array();
    for (auto& m : msgs)
        for (auto tid : m.tids) {
            auto t = transfers.find(tid);
            if (t != transfers.end()) files.push_back(ToJson(*t->second));
        }
    return files;
}

std::string Engine::ArchiveChat(const std::string& keyArg, bool clear, bool keepRecent) {
    auto lk = Lock();
    const std::string key = IsContactKey(keyArg) || IsGroupKey(keyArg) ? keyArg : ChatKey(keyArg);
    auto it = chats.find(key);
    if (it == chats.end() || it->second.messages.empty()) return "";
    const auto& msgs = it->second.messages;
    json body = json::object();
    json arr = json::array();
    for (auto& m : msgs) arr.push_back(ToJson(m));
    body["msgs"] = arr;
    body["files"] = HistoryFiles(msgs);
    int64_t created = NowMs();
    json head = {{"peer", key}, {"name", PeerName(key)}, {"count", (int)msgs.size()}, {"from", msgs.front().ts},
                 {"to", msgs.back().ts}, {"created", created}};
    std::string data = body.dump();
    bool z = false;
    if (compress) {
        std::string c = compress(data);
        if (!c.empty()) {
            data.swap(c);
            z = true;
        }
    }
    head["z"] = z;
    std::string dir = ArchDir(*this);
    MakeDirs(dir);
    std::string tag;
    for (char c : key) tag += isalnum((unsigned char)c) ? c : '_';
    if (tag.size() > 16) tag = tag.substr(tag.size() - 16);
    std::string path = UniquePath(JoinPath(dir, Stamp(created) + "-" + tag + ".lsarch"));
    std::string file = std::string(kArchMagic) + "\n" + head.dump() + "\n" + data;
    FILE* f = OpenFile(path, "wb");
    bool ok = f && fwrite(file.data(), 1, file.size(), f) == file.size();
    if (f) ok = fclose(f) == 0 && ok;
    if (!ok) {
        RemoveFile(path);
        return "";
    }
    if (clear) {  // what was archived goes (kept: yesterday's and today's, if asked, and what's still to be sent)
        int64_t keepFrom = keepRecent ? StartOfYesterday() : INT64_MAX;
        auto& v = it->second.messages;
        v.erase(std::remove_if(v.begin(), v.end(), [&](const ChatMessage& m) {
                    bool unsent = m.outgoing && m.kind != MsgKind::System && !m.pendingTo.empty();
                    return m.ts < keepFrom && !unsent;
                }),
                v.end());
        it->second.unread = 0;
    }
    MarkDirty();
    Emit({EventType::Info, key, ""});
    return path;
}

static bool ReadArchive(const std::string& path, json& head, std::string& rest) {
    FILE* f = OpenFile(path, "rb");
    if (!f) return false;
    std::string all;
    char buf[1 << 16];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) all.append(buf, n);
    fclose(f);
    size_t a = all.find('\n');
    if (a == std::string::npos || all.compare(0, a, kArchMagic) != 0) return false;
    size_t b = all.find('\n', a + 1);
    if (b == std::string::npos) return false;
    head = json::parse(all.substr(a + 1, b - a - 1), nullptr, false);
    if (!head.is_object()) return false;
    rest = all.substr(b + 1);
    return true;
}

std::vector<Engine::ChatArchiveInfo> Engine::ChatArchives(const std::string& keyArg) {
    std::string key;
    {
        auto lk = Lock();
        key = keyArg.empty() || IsContactKey(keyArg) || IsGroupKey(keyArg) ? keyArg : ChatKey(keyArg);
    }
    std::vector<ChatArchiveInfo> v;
    std::string dir = ArchDir(*this);
    for (auto& e : ListDir(dir)) {
        if (e.isDir || e.name.size() < 7 || e.name.compare(e.name.size() - 7, 7, ".lsarch") != 0) continue;
        std::string p = JoinPath(dir, e.name);
        // (only the first lines are needed)
        FILE* f = OpenFile(p, "rb");
        if (!f) continue;
        std::string top;
        char buf[4096];
        size_t n;
        while (top.find('\n', top.find('\n') + 1) == std::string::npos && (n = fread(buf, 1, sizeof buf, f)) > 0 && top.size() < (1 << 20))
            top.append(buf, n);
        fclose(f);
        size_t a = top.find('\n'), b = a == std::string::npos ? a : top.find('\n', a + 1);
        if (b == std::string::npos || top.compare(0, a, kArchMagic) != 0) continue;
        json h = json::parse(top.substr(a + 1, b - a - 1), nullptr, false);
        if (!h.is_object()) continue;
        ChatArchiveInfo i;
        i.path = p;
        i.peer = h.value("peer", "");
        i.name = h.value("name", "");
        i.count = h.value("count", 0);
        i.from = h.value("from", (int64_t)0);
        i.to = h.value("to", (int64_t)0);
        i.created = h.value("created", (int64_t)0);
        i.bytes = e.size;
        if (key.empty() || i.peer == key) v.push_back(i);
    }
    std::sort(v.begin(), v.end(), [](const ChatArchiveInfo& a, const ChatArchiveInfo& b) { return a.created > b.created; });
    return v;
}

bool Engine::DeleteChatArchive(const std::string& path) {
    if (DirName(path) != ArchDir(*this)) return false;
    return RemoveFile(path);
}

// messages into a conversation: those it doesn't have yet, in time order; their files' records too (a file
// that isn't on this device any more shows as gone). remote: from the other side (already flipped to my view).
int Engine::MergeHistory(const std::string& key, std::vector<ChatMessage> msgs, const json& files, const std::string& remoteDev) {
    Conversation& cv = chats[key];
    cv.peer = key;
    std::set<std::string> have;
    for (auto& m : cv.messages) have.insert(m.mid);
    std::map<uint64_t, json> fj;
    if (files.is_array())
        for (auto& f : files)
            if (f.is_object()) fj[f.value("tid", (uint64_t)0)] = f;
    int added = 0;
    for (auto& m : msgs) {
        if (m.mid.empty() || have.count(m.mid)) continue;
        have.insert(m.mid);
        m.pendingTo.clear();
        m.mailedTo.clear();
        m.reactionSent = true;
        m.readSent = true;
        if (m.outgoing) m.delivered = true;
        for (auto tid : m.tids) {
            if (transfers.count(tid)) continue;
            auto f = fj.find(tid);
            if (f == fj.end()) continue;
            auto t = std::make_shared<Transfer>();
            TransferFromJson(f->second, *t);
            if (!t->tid) continue;
            if (!remoteDev.empty()) {  // their record: my side of it is the other way round, and my files are elsewhere
                t->outgoing = !t->outgoing;
                t->peer = remoteDev;
                t->partPath.clear();
                std::string guess = JoinPath(settings.downloadDir, t->rel.empty() ? t->name : t->rel);
                t->localPath = FileSize(guess) == t->size ? guess : "";
            }
            bool here = !t->localPath.empty() && FileSize(t->localPath) == t->size;
            t->state = here ? TState::Completed : TState::Cancelled;
            if (!here) t->error = "not on this device";
            t->done = here ? t->size : 0;
            transfers[tid] = t;
        }
        cv.messages.push_back(m);
        added++;
    }
    std::stable_sort(cv.messages.begin(), cv.messages.end(), [](const ChatMessage& a, const ChatMessage& b) { return a.ts < b.ts; });
    if (added) {
        MarkDirty();
        Emit({EventType::Info, key, ""});
    }
    return added;
}

int Engine::RestoreChatArchive(const std::string& path) {
    json head;
    std::string rest;
    if (!ReadArchive(path, head, rest)) return -1;
    if (head.value("z", false)) {
        if (!decompress) return -1;
        rest = decompress(rest);
        if (rest.empty()) return -1;
    }
    json body = json::parse(rest, nullptr, false);
    if (!body.is_object()) return -1;
    std::vector<ChatMessage> msgs;
    for (auto& mj : body.value("msgs", json::array()))
        if (mj.is_object()) msgs.push_back(MessageFromJson(mj));
    auto lk = Lock();
    std::string key = head.value("peer", "");
    if (key.empty()) return -1;
    int n = MergeHistory(key, msgs, body.value("files", json::array()), "");
    if (n > 0) AddSystemMessage(key, "Restored " + std::to_string(n) + " message(s) from an archive.");
    return n;
}

// ---------------------------------------------------------------- asking the other side

void Engine::AskHistory(const std::string& keyArg) {
    auto lk = Lock();
    const std::string key = IsContactKey(keyArg) || IsGroupKey(keyArg) ? keyArg : ChatKey(keyArg);
    json j = KeyField(key, {{"chat", key}});
    std::vector<std::string> to = DevicesOf(key);
    for (auto& d : MyOtherDevices()) to.push_back(d);  // (my other devices have the same chats)
    std::sort(to.begin(), to.end());
    to.erase(std::unique(to.begin(), to.end()), to.end());
    int asked = 0;
    for (auto& d : to)
        if (d != settings.deviceId && QueueToPeer(d, Msg::HistoryAsk, j)) asked++;
    AddSystemMessage(key, asked ? "You asked for the history of this chat to be sent back." : "Nobody of this chat to ask right now.");
    Emit({EventType::Info, key, ""});
}

void Engine::AnswerHistoryAsk(const std::string& dev, const std::string& chat, bool send) {
    auto lk = Lock();
    auto it = std::find_if(historyAsks.begin(), historyAsks.end(), [&](const HistoryAsk& a) { return a.dev == dev && a.key == chat; });
    if (it == historyAsks.end()) return;
    HistoryAsk a = *it;
    historyAsks.erase(it);
    auto cv = chats.find(a.key);
    if (!send || cv == chats.end()) {
        QueueToPeer(a.dev, Msg::HistoryData, KeyField(a.key, {{"chat", a.chat}, {"declined", !send}, {"last", true}, {"msgs", json::array()}}));
        return;
    }
    // in parts (a long chat is a lot): the other side puts them together
    const auto& v = cv->second.messages;
    size_t per = 300;
    for (size_t i = 0; i < v.size() || i == 0; i += per) {
        std::vector<ChatMessage> part(v.begin() + (long)std::min(i, v.size()), v.begin() + (long)std::min(i + per, v.size()));
        json arr = json::array();
        for (auto& m : part)
            if (m.kind != MsgKind::System) arr.push_back(ToJson(m));
        bool last = i + per >= v.size();
        QueueToPeer(a.dev, Msg::HistoryData,
                    KeyField(a.key, {{"chat", a.chat}, {"msgs", arr}, {"files", HistoryFiles(part)}, {"last", last}, {"from", MyAccount()}}));
        if (last) break;
    }
    AddSystemMessage(a.key, "You sent the history of this chat back to " + a.name + ".");
}

void Engine::HandleHistory(const std::shared_ptr<Connection>& c, Msg type, const json& j) {
    const std::string& dev = c->peerId;
    auto dv = devices.find(dev);
    if (dv == devices.end()) return;
    bool mine = dv->second.accountPub == MyAccount();  // (my other device: the same chats, the same sides)
    std::string key = j.contains("gid") ? FrameKey(c, j) : mine ? j.value("chat", "") : ChatKey(dev);
    if (key.empty() || (mine && !IsContactKey(key) && !IsGroupKey(key) && !devices.count(key))) return;
    if (type == Msg::HistoryAsk) {
        auto cv = chats.find(key);
        int n = cv == chats.end() ? 0 : (int)cv->second.messages.size();
        historyAsks.erase(std::remove_if(historyAsks.begin(), historyAsks.end(), [&](const HistoryAsk& a) { return a.dev == dev && a.key == key; }),
                          historyAsks.end());
        HistoryAsk a{dev, key, mine ? PeerName(dev) : PeerName(ChatKey(dev)), j.value("chat", ""), SteadyMs(), n};
        if (n == 0) {  // nothing to give
            QueueToPeer(dev, Msg::HistoryData, KeyField(key, {{"chat", a.chat}, {"msgs", json::array()}, {"last", true}, {"empty", true}}));
            return;
        }
        historyAsks.push_back(a);
        if (historyAsks.size() > 20) historyAsks.erase(historyAsks.begin());
        std::string text = a.name + " asks for the history of this chat (they lost theirs).";
        AddSystemMessage(key, text);
        Emit({EventType::ChatReceived, key, text});
        return;
    }
    // HistoryData: their messages, turned to my side
    std::string from = j.value("from", "");  // (their account)
    std::vector<ChatMessage> msgs;
    for (auto& mj : j.value("msgs", json::array())) {
        if (!mj.is_object()) continue;
        ChatMessage m = MessageFromJson(mj);
        if (!mine) {
            if (IsGroupKey(key)) {
                if (m.outgoing) {  // theirs
                    m.outgoing = false;
                    m.from = from.empty() ? "" : "@" + from;
                } else if (m.from == "@" + MyAccount()) {  // mine
                    m.outgoing = true;
                    m.from.clear();
                }
            } else {
                m.outgoing = !m.outgoing;
                m.from.clear();
            }
            std::swap(m.myReaction, m.theirReaction);
            m.read = true;
        }
        msgs.push_back(m);
    }
    int n = MergeHistory(key, msgs, j.value("files", json::array()), mine ? "" : dev);
    historyGot_[key] += n;
    if (j.value("last", false)) {
        int total = historyGot_[key];
        historyGot_.erase(key);
        std::string who = mine ? PeerName(dev) : PeerName(ChatKey(dev));
        if (j.value("declined", false)) AddSystemMessage(key, who + " didn't send the history.");
        else if (j.value("empty", false)) AddSystemMessage(key, who + " has no history of this chat either.");
        else AddSystemMessage(key, "Restored " + std::to_string(total) + " message(s) sent back by " + who + ".");
        Emit({EventType::Info, key, ""});
    }
}

}  // namespace ls
