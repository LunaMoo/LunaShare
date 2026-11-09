#include "notify.h"

#include <atomic>
#include <deque>
#include <mutex>

#include "bluetooth.h"
#include "engine.h"
#include "i18n.h"

namespace notify {

static std::atomic<bool> g_bg{false};
static std::mutex g_m;
static std::deque<Action> g_actions;
static TrayHook g_tray = nullptr;

void SetBackground(bool bg) { g_bg = bg; }
bool Background() { return g_bg; }
void SetTrayHook(TrayHook h) { g_tray = h; }

void Push(const Action& a) {
    std::lock_guard<std::mutex> l(g_m);
    if (g_actions.size() < 32) g_actions.push_back(a);
}

bool Pop(Action& a) {
    std::lock_guard<std::mutex> l(g_m);
    if (g_actions.empty()) return false;
    a = g_actions.front();
    g_actions.pop_front();
    return true;
}

bool Peek() {
    std::lock_guard<std::mutex> l(g_m);
    return !g_actions.empty();
}

void ClearChat(const std::string& key) {
    if (ls::Bridge().clearChatNotification) ls::Bridge().clearChatNotification(key);
}

void Attach(ls::Engine* eng) {
    eng->onEvent = [eng](const ls::Event& e) {
        // (engine thread, engine lock held)
        auto& b = ls::Bridge();
        switch (e.type) {
            case ls::EventType::ChatReceived: {
                if (!g_bg || eng->ChatMuted(e.peer)) return;
                std::string text = e.text.size() > 300 ? e.text.substr(0, 297) + "..." : e.text;
                if (!eng->settings.lockHash.empty()) text = T("New message");  // (the app lock: no text outside the app)
                if (b.notifyMessage) b.notifyMessage(e.peer, eng->PeerName(e.peer), text, eng->AvatarPath(e.peer));
                else if (g_tray) g_tray(eng->PeerName(e.peer), text);
                break;
            }
            case ls::EventType::ChatReaction:
                if (g_bg && b.notifyMessage && !eng->ChatMuted(e.peer))
                    b.notifyMessage(e.peer, eng->PeerName(e.peer),
                                    eng->settings.lockHash.empty() ? TF("reacted %s", e.text.c_str()) + "  " + e.a : std::string(T("New reaction")),
                                    eng->AvatarPath(e.peer));
                break;
            case ls::EventType::CallIncoming: {
                if (!g_bg) return;
                std::string key = eng->ChatKey(e.peer);
                bool video = eng->call.media == "video";
                if (b.notifyCall) b.notifyCall(key, eng->PeerName(key), video, eng->AvatarPath(key));
                else if (g_tray) g_tray(eng->PeerName(key), video ? T("is video calling you") : T("is calling you"));
                break;
            }
            case ls::EventType::CallStarted:
                if (b.cancelCallNotification) b.cancelCallNotification();
                break;
            case ls::EventType::CallEnded: {
                if (b.cancelCallNotification) b.cancelCallNotification();
                bool missed = e.text.rfind("Missed call", 0) == 0;
                if (missed && g_bg && b.notifyMessage) {
                    std::string key = eng->ChatKey(e.peer);
                    b.notifyMessage(key, eng->PeerName(key), TDynamic(e.text), eng->AvatarPath(key));
                }
                break;
            }
            case ls::EventType::LinkRequest:
                if (g_bg && b.notifyMessage) b.notifyMessage(e.peer, e.a, T("Link this device?"), "");
                break;
            default:
                break;
        }
    };
}

}  // namespace notify
