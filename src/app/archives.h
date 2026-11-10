// LunaShare - archives through libarchive: what's inside, unpacking (all or some of it), packing files and folders.
// Reads zip, 7z, rar (4 and 5), tar (plain, .gz, .bz2, .xz, .zst), gz / bz2 / xz / zst, iso, cab, lha, cpio, ar and
// the comic archives (cbz, cbr, cb7, cbt); writes zip, 7z and tar.gz / tar.xz / tar.zst. Paths are UTF-8; unpacking
// never writes outside the folder chosen (no absolute paths, no "..", no links).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace arc {

bool IsArchivePath(const std::string& path);  // (by its name; comics included)
bool IsComicPath(const std::string& path);    // cbz / cbr / cb7 / cbt

struct Entry {
    std::string path;  // inside the archive, '/' between folders, no trailing '/'
    int64_t size = 0, mtime = 0;  // mtime: unix seconds
    bool dir = false, encrypted = false;
};
struct Listing {
    std::vector<Entry> entries;  // (folders that only appear in paths are added)
    std::string format;          // e.g. "ZIP 2.0 (deflation)", "RAR5", "7-Zip"
    bool encrypted = false;      // some names or contents need a password
    bool needPassword = false;   // ... and the one given (if any) didn't do
    std::string error;
};
bool List(const std::string& path, Listing& out, const std::string& password = "");

// progress(done bytes, total bytes, current name): false = stop
using Progress = std::function<bool(int64_t, int64_t, const std::string&)>;

// only: entry paths to unpack (a folder brings what's in it); empty = everything. strip: a leading folder path to
// leave out of where they go ("a/b" -> a/b/c.txt lands as c.txt). written: the files made (full paths).
bool Extract(const std::string& path, const std::string& dest, const std::vector<std::string>& only, const std::string& password,
             const Progress& progress, std::string& err, std::vector<std::string>* written = nullptr, const std::string& strip = "");

enum class Format { Zip, SevenZip, TarGz, TarXz, TarZst };
const char* FormatExt(Format f);  // ".zip", ".7z", ".tar.gz"...
// files and folders (a folder goes in with its name and everything in it)
bool Create(const std::string& out, Format f, const std::vector<std::string>& inputs, const Progress& progress, std::string& err);

// a name made safe to put under a folder: '/' between parts, no "", ".", ".." parts, no drive, no absolute path
std::string SafeRelPath(const std::string& p);

}  // namespace arc
