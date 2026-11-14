// Scriptable headless peer used for demos/screenshots: demo_peer <cfgdir> <name> <host> <port> [files...]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include <cmath>

#include "engine.h"
#include "platform.h"
#ifdef LS_DEMO_VOICE
#include "lunashare_media.h"
#endif
#ifdef LS_DEMO_VIDEO
#include "lunashare_video.h"
#endif

using namespace ls;

int main(int argc, char** argv) {
    if (argc < 5) { printf("usage: demo_peer <cfgdir> <name> <host> <port> [files...]\n"); return 1; }
    MakeDirs(argv[1]);
    json j = {{"settings", {{"displayName", argv[2]}, {"downloadDir", JoinPath(argv[1], "dl")}, {"tcpPort", 48300},
                            {"discovery", getenv("DEMO_DISCOVERY") != nullptr}, {"autoConnect", false}, {"autoAcceptAll", true},
                            {"speedLimitKBs", 1500}}}};
    json existing;
    if (!LoadJsonFile(JoinPath(argv[1], "settings.json"), existing)) SaveJsonFile(JoinPath(argv[1], "settings.json"), j);
    Engine e;
    e.configDir = argv[1];
    std::string callKind = getenv("DEMO_CALL") ? getenv("DEMO_CALL") : "";
    bool videoCall = callKind == "video" || callKind == "webcam-offer";
    if (getenv("DEMO_CALL")) e.localCaps = {"voice"};
#ifdef LS_DEMO_VIDEO
    if (videoCall) e.localCaps.push_back("video");
#endif
    e.Start();
    if (getenv("DEMO_SHARE")) {  // a few files in its Shared folder (screenshots of the library)
        const char* names[] = {"Holiday 2026/beach.jpg", "Holiday 2026/mountains.jpg", "Holiday 2026/video.mp4", "Recipes/pierogi.pdf", "music/song.ogg"};
        int64_t sizes[] = {3400000, 2900000, 48000000, 420000, 5100000};
        for (int i = 0; i < 5; i++) {
            std::string p = JoinPath(e.SharedDir(), names[i]);
            MakeDirs(DirName(p));
            if (FILE* f = fopen(p.c_str(), "wb")) {
                std::vector<char> buf(1 << 16, (char)i);
                for (int64_t left = sizes[i]; left > 0; left -= (int64_t)buf.size()) fwrite(buf.data(), 1, (size_t)std::min<int64_t>(left, (int64_t)buf.size()), f);
                fclose(f);
            }
        }
        e.RescanShared();
    }
    if (const char* av = getenv("DEMO_AVATAR")) {  // a profile picture (JPEG file) for screenshots
        std::string jpeg;
        if (FILE* f = fopen(av, "rb")) {
            char buf[8192];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) jpeg.append(buf, n);
            fclose(f);
        }
        e.SetAvatar(jpeg);
    }
    if (std::string(argv[3]) == "-") {  // just stay online (discovery testing)
        std::this_thread::sleep_for(std::chrono::seconds(getenv("DEMO_SECONDS") ? atoi(getenv("DEMO_SECONDS")) : 30));
        e.Stop();
        return 0;
    }
    std::string peer;
    for (int i = 0; i < 150 && peer.empty(); i++) {
        if (i % 10 == 0) e.ConnectAddress(argv[3], (uint16_t)atoi(argv[4]));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto lk = e.Lock();
        for (auto& c : e.conns) if (c->ready) peer = c->peerId;
    }
    if (peer.empty()) { printf("could not connect\n"); return 1; }
    if (getenv("DEMO_CALL")) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        std::string target = peer;
        if (getenv("DEMO_GROUP")) {  // a group call
            std::string them;
            {
                auto l = e.Lock();
                them = e.ChatKey(peer);
            }
            target = e.CreateGroup(u8"Rodzina \U0001F3E1", {them});
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        uint64_t cid = e.StartCall(target, callKind == "webcam-offer" ? "webcam-offer" : videoCall ? "video" : "voice");
        (void)cid;
        fflush(stdout);
#ifdef LS_DEMO_VOICE
        // once accepted, talk: a warbling tone through the real voice module
        const LsmModule* m = lunashare_media_module();
        static Engine* eng = &e;
        LsmHost h{LSM_API_VERSION, nullptr, [](void*, const uint8_t* d, size_t n) { eng->SendMedia(d, n); }, nullptr};
        LsmSettings st{0, 0, 0, 24000};
        bool started = false;
        int64_t t = 0;
        int secs = getenv("DEMO_SECONDS") ? atoi(getenv("DEMO_SECONDS")) : 30;
        for (int i = 0; i < secs * 50; i++) {
            bool active;
            {
                auto lk = e.Lock();
                active = e.call.state == CallState::Active;
            }
            if (active && !started) {
                m->start(&h, &st);
                started = true;
#ifdef LS_DEMO_VIDEO
                if (videoCall) {
                    // test-pattern camera through the video module core
                    static LsvHost vh{LSV_API_VERSION, nullptr,
                                      [](void*, const uint8_t* d, size_t n) { return eng->SendMedia(d, n) ? 1 : 0; },
                                      [](void*, int, const LsvImage*) {}, [](void*, int) {},
                                      [](void*) { return eng->MediaQueuedBytes(); }, nullptr, nullptr, nullptr,
                                      [](void*) { return eng->CallLinkBps(); }};
                    const LsvModule* vm = lunashare_video_module();
                    LsvSettings vs{960, 540, 24, 1800000, 1};
                    vm->init(&vh);
                    vm->start(&vs);
                    vm->set_camera_on(1);
                    eng->onMedia = [vm](uint64_t, int, const uint8_t* d, size_t n) { if (n && d[0] == 2) vm->receive(d, n); };
                }
#endif
            }
            static bool readOffered = false;  // DEMO_READ during the call: offer the comic a moment after it's answered
            if (started && !readOffered && getenv("DEMO_READ") && i % 150 == 0) {
                readOffered = true;
                std::string them;
                {
                    auto l = e.Lock();
                    them = e.ChatKey(peer);
                }
                std::vector<std::string> pages;
                for (auto& d : ListDir(getenv("DEMO_READ")))
                    if (!d.isDir) pages.push_back(JoinPath(getenv("DEMO_READ"), d.name));
                std::sort(pages.begin(), pages.end());
                e.StartRead(them, "Demo comic.cbz", pages, 0);
            }
            if (started) {
                int16_t f[320];
                for (int k = 0; k < 320; k++, t++)
                    f[k] = (int16_t)(7000 * (0.6 + 0.4 * std::sin(t * 0.0008)) * std::sin(2 * 3.14159 * (300 + 80 * std::sin(t * 0.0003)) * t / 16000.0));
                m->capture(f, 320);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        e.Stop();
        return 0;
#endif
    }
    e.SendChat(peer, "Hi! Sending you the photos from Saturday :)");
    std::vector<std::string> imgs, other;
    for (int i = 5; i < argc; i++) (std::string(argv[i]).find(".bin") != std::string::npos ? other : imgs).push_back(argv[i]);
    if (!imgs.empty()) e.SendPaths(peer, imgs);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    e.SendChat(peer, u8"Zażółć gęślą jaźń - Polish characters work too. Long-press or right-click a message to copy it.");
    e.SendChat(peer, "Links open in your browser: www.google.pl and https://github.com/LunaMoo/LunaShare (tap them).");
    {  // an answer, then a changed message
        std::string q;
        {
            auto l = e.Lock();
            auto& ms = e.chats[e.ChatKey(peer)].messages;
            q = ms[ms.size() - 2].mid;
        }
        e.SendChat(peer, u8"Answering myself: this is how a reply looks.", q);
        e.SendChat(peer, "This one gets changed");
        std::string c;
        {
            auto l = e.Lock();
            c = e.chats[e.ChatKey(peer)].messages.back().mid;
        }
        e.EditMessage(peer, c, "This one was changed afterwards");
    }
    if (!other.empty()) e.SendPaths(peer, other);
    if (getenv("DEMO_EMOJI")) {
        e.SendChat(peer, u8"Emoji too \U0001F389 \U0001F44D\U0001F3FD family \U0001F468\u200D\U0001F469\u200D\U0001F467 flag \U0001F1F5\U0001F1F1 keycap 1\uFE0F\u20E3 and text \u00A9 stays text");
        e.SendChat(peer, u8"\u2764\uFE0F");
        e.SendChat(peer, u8"\U0001F602\U0001F602\U0001F525");
        std::string first, second;
        {
            auto l = e.Lock();
            auto& ms = e.chats[e.ChatKey(peer)].messages;
            for (auto& m : ms)
                if (m.kind == MsgKind::Text) {
                    if (first.empty()) first = m.mid;
                    else if (second.empty()) second = m.mid;
                }
        }
        e.React(peer, first, u8"\u2764\uFE0F");
        e.React(peer, second, u8"\U0001F44D");
    }
    if (const char* mv = getenv("DEMO_WATCH")) {  // watch a movie together with the other side
        std::string them;
        for (int i = 0; i < 50 && them.empty(); i++) {
            {
                auto l = e.Lock();
                std::string k = e.ChatKey(peer);
                if (Engine::IsContactKey(k)) them = k;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        e.StartWatch(them, mv);
        if (const char* hs = getenv("DEMO_HOSTSUB")) e.WatchSetHostSub(hs);  // as if the host had these subtitles on
        if (const char* next = getenv("DEMO_WATCH_SWAP")) {  // a few seconds later: another movie instead
            std::this_thread::sleep_for(std::chrono::milliseconds(4000));
            e.LeaveWatch();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            e.StartWatch(them, next);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
        e.WatchControl(true, getenv("DEMO_WATCH_AT") ? atof(getenv("DEMO_WATCH_AT")) : 20);  // (the clock; the demo itself doesn't decode)
    }
    if (const char* rd = getenv("DEMO_READ")) {  // read a comic (a folder of pictures) together with the other side
        std::string them;
        for (int i = 0; i < 50 && them.empty(); i++) {
            {
                auto l = e.Lock();
                std::string k = e.ChatKey(peer);
                if (Engine::IsContactKey(k)) them = k;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::vector<std::string> pages;
        for (auto& d : ListDir(rd))
            if (!d.isDir) pages.push_back(JoinPath(rd, d.name));
        std::sort(pages.begin(), pages.end());
        e.StartRead(them, "Demo comic.cbz", pages, 0);
        if (const char* t = getenv("DEMO_READ_TURN")) {  // a while later: the next page, for everyone
            std::this_thread::sleep_for(std::chrono::milliseconds(atoi(t)));
            e.ReadTurn(2);
        }
    }
    if (getenv("DEMO_GROUP")) {  // a group with the other side (screenshots of group chats)
        std::string them;
        for (int i = 0; i < 50 && them.empty(); i++) {
            {
                auto l = e.Lock();
                std::string k = e.ChatKey(peer);
                if (Engine::IsContactKey(k)) them = k;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::string g = e.CreateGroup(u8"Rodzina 🏡", {them});
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        e.SendChat(g, u8"Cześć wszystkim! Tu jest nasza grupa.");
        e.SendChat(g, u8"W niedzielę obiad u babci o 14:00 🍽");
    }
    int secs = getenv("DEMO_SECONDS") ? atoi(getenv("DEMO_SECONDS")) : 30;
    for (int64_t end = SteadyMs() + secs * 1000; SteadyMs() < end;) {
        if (getenv("DEMO_READ_JOIN")) {  // join the other side's comic as soon as it's offered
            uint64_t sid = 0;
            {
                auto l = e.Lock();
                if (!e.read.sid && !e.readInvites.empty()) sid = e.readInvites.back().sid;
            }
            if (sid) e.JoinRead(sid);
        }
        for (auto& ev : e.PopEvents())  // DEMO_LINK: accept a request to join the other side's account
            if (ev.type == EventType::LinkRequest && getenv("DEMO_LINK")) {
                printf("link request, code %s\n", ev.text.c_str());
                e.AnswerLink(ev.peer, true);
            } else if (ev.type == EventType::LinkDone) {
                printf("%s\n", ev.text.c_str());
            }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    e.Stop();
    return 0;
}
