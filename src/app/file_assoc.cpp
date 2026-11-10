#include "file_assoc.h"

#include <cstdlib>
#include <filesystem>

#include "platform.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#elif defined(__linux__) && !defined(__ANDROID__)
#include <unistd.h>
#endif

using namespace ls;
namespace fs = std::filesystem;

namespace assoc {

const std::vector<Kind>& Kinds() {
    static const std::vector<Kind> k = {
        {"Image", "Picture", {"jpg", "jpeg", "jfif", "png", "gif", "bmp", "webp", "tga", "psd"},
         {"image/jpeg", "image/png", "image/gif", "image/bmp", "image/webp", "image/x-tga", "image/vnd.adobe.photoshop"}},
        {"Video", "Movie", {"mp4", "m4v", "mov", "mkv", "webm", "avi", "3gp"},
         {"video/mp4", "video/x-m4v", "video/quicktime", "video/x-matroska", "video/webm", "video/x-msvideo", "video/3gpp"}},
        {"Audio", "Song", {"mp3", "m4a", "aac", "flac", "ogg", "oga", "opus", "wav", "wma", "aiff", "aif", "ape", "wv", "mka"},
         {"audio/mpeg", "audio/mp4", "audio/aac", "audio/flac", "audio/ogg", "audio/opus", "audio/x-wav", "audio/x-ms-wma", "audio/x-aiff",
          "audio/x-ape", "audio/x-wavpack", "audio/x-matroska"}},
        {"Archive", "Archive", {"zip", "7z", "rar", "tar", "gz", "tgz", "xz", "txz", "bz2", "tbz2", "zst", "tzst", "iso", "cab", "lzh", "lha", "cpio"},
         {"application/zip", "application/x-7z-compressed", "application/vnd.rar", "application/x-rar", "application/x-tar", "application/gzip",
          "application/x-compressed-tar", "application/x-xz", "application/x-xz-compressed-tar", "application/x-bzip2",
          "application/x-bzip-compressed-tar", "application/zstd", "application/x-zstd-compressed-tar", "application/x-iso9660-image",
          "application/vnd.ms-cab-compressed", "application/x-lha", "application/x-cpio"}},
        {"Comic", "Comic book", {"cbz", "cbr", "cb7", "cbt"},
         {"application/vnd.comicbook+zip", "application/vnd.comicbook-rar", "application/x-cbz", "application/x-cbr", "application/x-cb7",
          "application/x-cbt"}},
    };
    return k;
}

#ifdef _WIN32
static std::wstring W(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
static std::wstring ExePath() {
    wchar_t buf[32768];
    DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
    return std::wstring(buf, n);
}
static bool SetStr(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& val) {
    HKEY h;
    if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr) != ERROR_SUCCESS) return false;
    LONG r = RegSetValueExW(h, name, 0, REG_SZ, (const BYTE*)val.c_str(), (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return r == ERROR_SUCCESS;
}
static std::wstring GetStr(HKEY root, const std::wstring& key, const wchar_t* name) {
    wchar_t buf[4096];
    DWORD sz = sizeof(buf);
    if (RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return L"";
    return buf;
}
static std::wstring Command() { return L"\"" + ExePath() + L"\" \"%1\""; }

bool Supported() { return true; }

bool Registered() { return GetStr(HKEY_CURRENT_USER, L"Software\\Classes\\LunaShare.Image\\shell\\open\\command", nullptr) == Command(); }

bool Register(std::string& err) {
    const std::wstring cls = L"Software\\Classes\\";
    std::wstring exe = ExePath(), cmd = Command();
    bool ok = true;
    // the program itself ("Open with" lists it, with its name)
    std::wstring app = cls + L"Applications\\LunaShare.exe";
    ok &= SetStr(HKEY_CURRENT_USER, app, L"FriendlyAppName", L"LunaShare");
    ok &= SetStr(HKEY_CURRENT_USER, app + L"\\shell\\open\\command", nullptr, cmd);
    // capabilities: the entry in Settings > Default apps
    std::wstring cap = L"Software\\LunaShare\\Capabilities";
    ok &= SetStr(HKEY_CURRENT_USER, cap, L"ApplicationName", L"LunaShare");
    ok &= SetStr(HKEY_CURRENT_USER, cap, L"ApplicationDescription", L"Chat, share files, pictures, movies, music, archives and comics");
    ok &= SetStr(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", L"LunaShare", cap);
    for (auto& k : Kinds()) {
        std::wstring prog = L"LunaShare." + W(k.id);
        ok &= SetStr(HKEY_CURRENT_USER, cls + prog, nullptr, L"LunaShare " + W(k.name));
        ok &= SetStr(HKEY_CURRENT_USER, cls + prog + L"\\DefaultIcon", nullptr, L"\"" + exe + L"\",0");
        ok &= SetStr(HKEY_CURRENT_USER, cls + prog + L"\\shell\\open\\command", nullptr, cmd);
        for (auto& e : k.exts) {
            std::wstring ext = L"." + W(e);
            ok &= SetStr(HKEY_CURRENT_USER, app + L"\\SupportedTypes", ext.c_str(), L"");
            ok &= SetStr(HKEY_CURRENT_USER, cls + ext + L"\\OpenWithProgids", prog.c_str(), L"");
            ok &= SetStr(HKEY_CURRENT_USER, cap + L"\\FileAssociations", ext.c_str(), prog);
        }
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    if (!ok) err = "the registry refused a change";
    return ok;
}

void Unregister() {
    const std::wstring cls = L"Software\\Classes\\";
    for (auto& k : Kinds()) {
        std::wstring prog = L"LunaShare." + W(k.id);
        RegDeleteTreeW(HKEY_CURRENT_USER, (cls + prog).c_str());
        for (auto& e : k.exts) {
            HKEY h;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, (cls + L"." + W(e) + L"\\OpenWithProgids").c_str(), 0, KEY_WRITE, &h) == ERROR_SUCCESS) {
                RegDeleteValueW(h, prog.c_str());
                RegCloseKey(h);
            }
        }
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, (cls + L"Applications\\LunaShare.exe").c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\LunaShare");
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", 0, KEY_WRITE, &h) == ERROR_SUCCESS) {
        RegDeleteValueW(h, L"LunaShare");
        RegCloseKey(h);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

bool CanOpenSystemSettings() { return true; }
void OpenSystemSettings() { OpenWithDefaultApp("ms-settings:defaultapps"); }

#elif defined(__linux__) && !defined(__ANDROID__)

static std::string DataHome() {
    const char* x = getenv("XDG_DATA_HOME");
    return x && *x ? std::string(x) : JoinPath(HomeDir(), ".local/share");
}
static std::string DesktopFile() { return JoinPath(DataHome(), "applications/lunashare.desktop"); }
static std::string ExePath() {
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : p.string();
}
static std::string Quote(const std::string& s) {  // (the .desktop Exec rules: quotes, backslashes, $ and `)
    std::string r = "\"";
    for (char c : s) {
        if (c == '"' || c == '`' || c == '$' || c == '\\') r += '\\';
        r += c;
    }
    return r + "\"";
}

bool Supported() { return !ExePath().empty(); }

bool Registered() {
    std::string s;
    FILE* f = OpenFile(DesktopFile(), "rb");
    if (!f) return false;
    char buf[8192];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    s.assign(buf, n);
    return s.find("Exec=" + Quote(ExePath())) != std::string::npos;
}

bool Register(std::string& err) {
    std::string exe = ExePath();
    if (exe.empty()) {
        err = "where the program is isn't known";
        return false;
    }
    std::string icon = "lunashare";
    std::string png = JoinPath(DirName(exe), "lunashare.png");  // (shipped next to the program)
    if (FileExists(png)) {
        std::string dir = JoinPath(DataHome(), "icons/hicolor/256x256/apps");
        MakeDirs(dir);
        std::error_code ec;
        fs::copy_file(png, JoinPath(dir, "lunashare.png"), fs::copy_options::overwrite_existing, ec);
    }
    std::string mimes;
    for (auto& k : Kinds())
        for (auto& m : k.mimes) mimes += m + ";";
    std::string d = "[Desktop Entry]\nType=Application\nName=LunaShare\nComment=Chat and share files; pictures, movies, music, archives and comics\n"
                    "Exec=" + Quote(exe) + " %F\nIcon=" + icon + "\nTerminal=false\nCategories=Network;FileTransfer;AudioVideo;Graphics;Utility;\n"
                    "MimeType=" + mimes + "\nStartupWMClass=LunaShare\n";
    MakeDirs(JoinPath(DataHome(), "applications"));
    FILE* f = OpenFile(DesktopFile(), "wb");
    if (!f) {
        err = "can't write " + DesktopFile();
        return false;
    }
    fwrite(d.data(), 1, d.size(), f);
    fclose(f);
    // (refresh the menu's cache; fine if the tool isn't there)
    std::string cmd = "update-desktop-database " + Quote(JoinPath(DataHome(), "applications")) + " >/dev/null 2>&1";
    if (system(cmd.c_str()) != 0) {}
    return true;
}

void Unregister() {
    RemoveFile(DesktopFile());
    std::string cmd = "update-desktop-database " + Quote(JoinPath(DataHome(), "applications")) + " >/dev/null 2>&1";
    if (system(cmd.c_str()) != 0) {}
}

bool CanOpenSystemSettings() { return false; }
void OpenSystemSettings() {}

#else

bool Supported() { return false; }
bool Registered() { return false; }
bool Register(std::string& err) {
    err = "not here";
    return false;
}
void Unregister() {}
bool CanOpenSystemSettings() { return false; }
void OpenSystemSettings() {}

#endif

}  // namespace assoc
