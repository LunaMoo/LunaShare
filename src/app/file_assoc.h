// LunaShare - "Open with": telling the system (Windows, Linux desktops) which files LunaShare can open, for the
// current user only (no administrator). Windows lists it under "Open with" and in Settings > Default apps (Windows
// doesn't let a program make itself the default: the user picks it there); Linux gets a .desktop file with the types.
// Android declares this in its manifest (always on).
#pragma once
#include <string>
#include <vector>

namespace assoc {

bool Supported();
bool Registered();             // (as this program, from where it is now)
bool Register(std::string& err);
void Unregister();
bool CanOpenSystemSettings();  // Windows: Settings > Default apps
void OpenSystemSettings();

// the file kinds: their extensions (lower case, no dot)
struct Kind {
    const char* id;    // Image, Video, Audio, Archive, Comic
    const char* name;  // shown by the system
    std::vector<std::string> exts;
    std::vector<std::string> mimes;  // (Linux)
};
const std::vector<Kind>& Kinds();

}  // namespace assoc
