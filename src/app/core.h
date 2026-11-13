// The engine of this process - the part that keeps people connected (messages, notifications, incoming calls).
//
// Computers: made when the window opens, ended when it closes.
// Phones: it outlives the screen. Android destroys the screen when it likes (swiped from the recent apps, memory
// reclaimed, a phone maker's "cleaner"); SDL then ends the UI's thread, and the engine used to end with it - nothing
// arrived until the app was opened again, though the background service kept the process alive. Now, with "Keep
// running in the background" on, the UI lets go of the engine without stopping it, and takes the running one back
// when the screen comes again. If Android restarted the process itself (or the phone was restarted), the background
// service starts the engine without a screen.
#pragma once
#include <string>

namespace ls {
class Engine;
}

namespace core {
ls::Engine* Running();  // the engine already running in this process, or null
// A new engine, set up the same way whoever starts it (the UI or the background service) - not started yet.
// Must be called with no engine running.
ls::Engine& Make();
// Remembers / restores what the engine announces to others (calls, video...): the background service starts it
// without the modules the UI loads, and others should still see that they can call.
void SaveCaps(ls::Engine& e);
void LoadCaps(ls::Engine& e);
// The UI is going away: should the engine keep running without it?
bool KeepWithoutScreen(ls::Engine& e);
void Destroy();  // stops and ends the engine

// Phones: the background service starts the engine without a screen (config: the app's files folder).
// false when it shouldn't run (background running switched off, or it couldn't start).
bool StartWithoutScreen(const std::string& filesDir);
// Dev / tests (computers): LUNASHARE_KEEP_CORE behaves like a phone - the window's end keeps the engine
bool Emulated();
}  // namespace core
