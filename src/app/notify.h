// System notifications while the app's window is not in front (phone in the pocket, app in the background, window
// closed to the tray), and actions coming back from them.
#pragma once
#include <string>

namespace ls { class Engine; }

namespace notify {

void Attach(ls::Engine* eng);   // hooks the engine's events (they arrive on engine threads)
void SetBackground(bool bg);    // the UI isn't visible (Android: paused; desktop: hidden / minimized)
bool Background();
void ClearChat(const std::string& key);  // the user opened this conversation

// actions from notifications ("open" / "answer"), queued for the UI thread
struct Action {
    std::string what, key;
};
void Push(const Action& a);  // any thread
bool Pop(Action& a);
bool Peek();  // an action is waiting (the window should come to the front)

// desktop tray (Windows): shows a balloon for messages / calls while the window is hidden
using TrayHook = void (*)(const std::string& title, const std::string& text);
void SetTrayHook(TrayHook h);

}  // namespace notify
