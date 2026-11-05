// Tray icon for running in the background on computers (Windows; elsewhere Available() is false and closing the
// window minimises it instead).
#pragma once
#include <string>

namespace tray {
bool Available();
bool Show(const std::string& tooltip);  // adds the icon (once)
void Hide();
void Balloon(const std::string& title, const std::string& text);  // any thread
bool TakeShow();  // the user clicked the icon / "Open"
bool TakeQuit();  // ... chose "Quit"
}  // namespace tray
