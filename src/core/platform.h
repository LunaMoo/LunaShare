// LunaShare - platform helpers (paths, identity hints, misc OS glue)
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ls {

// Human readable platform name: "Windows", "Linux", "macOS", "Android", "iOS".
const char* PlatformName();
bool IsMobilePlatform();

std::string HostName();
std::string UserName();
// a stable id of this computer / phone (survives reinstalling the app; empty if unknown). Android: set by the bridge.
std::string MachineId();
void SetMachineId(const std::string& id);

// Directory for settings/history (created if missing). Can be overridden (tests, portable mode).
std::string ConfigDir();
void SetConfigDirOverride(const std::string& dir);

// Default directory for received files.
std::string DefaultDownloadDir();
std::string HomeDir();

// Hardware (MAC) addresses of physical-ish interfaces, "aa:bb:cc:dd:ee:ff".
// Purely informational: phones randomise/hide them, so identity is a generated UUID.
std::vector<std::string> MacAddresses();

struct LocalAddress {
    std::string iface;
    std::string ip;         // dotted IPv4
    std::string broadcast;  // directed broadcast, may be empty
};
std::vector<LocalAddress> LocalIPv4Addresses();
// Likely IPv4 default gateways (routing table where readable, plus x.y.z.1 guesses for each local /24).
std::vector<std::string> DefaultGateways();

// Filesystem helpers (UTF-8 paths everywhere).
bool MakeDirs(const std::string& path);
bool FileExists(const std::string& path);
bool IsDirectory(const std::string& path);
int64_t FileSize(const std::string& path);
int64_t FileMTime(const std::string& path);  // seconds since epoch
bool RenameFile(const std::string& from, const std::string& to);
bool RemoveFile(const std::string& path);
std::string JoinPath(const std::string& a, const std::string& b);
std::string BaseName(const std::string& path);
std::string DirName(const std::string& path);
// Removes "..", absolute prefixes, drive letters and illegal characters from a remote supplied relative path.
std::string SanitizeRelativePath(const std::string& rel);
// "file.txt" -> "file (1).txt" when the path already exists.
std::string UniquePath(const std::string& path);

struct DirEntry {
    std::string name;
    bool isDir = false;
    int64_t size = 0;
    int64_t mtime = 0;  // unix seconds (for display / sorting)
};
std::vector<DirEntry> ListDir(const std::string& path);
// Recursively collects regular files; relative paths use '/' and start with the folder's own name.
void CollectFiles(const std::string& root, std::vector<std::pair<std::string, std::string>>& out /*abs, rel*/);

// Opens the folder (or reveals a file) in the system file manager.
void OpenInFileManager(const std::string& path);
void OpenWithDefaultApp(const std::string& path);

// stdio with UTF-8 paths (wide on Windows).
FILE* OpenFile(const std::string& path, const char* mode);
bool FileSeek(FILE* f, int64_t off);

int64_t NowMs();       // wall clock, ms since epoch
int64_t SteadyMs();    // monotonic
std::string FormatTime(int64_t ms, bool withDate = true);
std::string FormatBytes(double bytes);
std::string RandomHex(int bytes);
uint64_t RandomU64();

}  // namespace ls
