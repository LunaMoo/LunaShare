#include "instance.h"

#include <filesystem>

#include "model.h"
#include "platform.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace ls;
using json = nlohmann::json;

namespace instance {

static std::string ReqDir(const std::string& dir) { return JoinPath(dir, "requests"); }

bool Acquire(const std::string& configDir) {
    MakeDirs(configDir);
#ifdef _WIN32
    // a named mutex per folder (the name can't hold backslashes)
    std::string name = "Local\\LunaShare-" + std::to_string(std::hash<std::string>()(configDir));
    HANDLE h = CreateMutexA(nullptr, FALSE, name.c_str());
    if (!h) return true;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(h);
        return false;
    }
    return true;  // (kept open until the process ends)
#else
    std::string p = JoinPath(configDir, "instance.lock");
    int fd = open(p.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) return true;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return false;
    }
    return true;  // (the descriptor stays open: the lock lasts as long as this copy)
#endif
}

void SendRequest(const std::string& configDir, const std::vector<std::string>& paths, bool send) {
    json j;
    j["paths"] = json::array();
    for (auto& p : paths) {
        std::error_code ec;
        fs::path a = fs::absolute(fs::u8path(p), ec);
        j["paths"].push_back(ec ? p : a.u8string());
    }
    j["send"] = send;
    std::string d = ReqDir(configDir);
    MakeDirs(d);
    // (written under another name, then renamed: the running copy never reads half a file)
    std::string name = std::to_string(NowMs()) + "-" + std::to_string(rand() % 100000);
    SaveJsonFile(JoinPath(d, name + ".json"), j);
#ifdef _WIN32
    AllowSetForegroundWindow(ASFW_ANY);  // (the running copy may bring its window to the front)
#endif
}

bool Poll(const std::string& configDir, Request& r) {
    std::string d = ReqDir(configDir);
    std::vector<std::string> names;
    for (auto& e : ListDir(d))
        if (!e.isDir && e.name.size() > 5 && e.name.compare(e.name.size() - 5, 5, ".json") == 0) names.push_back(e.name);
    if (names.empty()) return false;
    std::sort(names.begin(), names.end());
    std::string p = JoinPath(d, names[0]);
    json j;
    bool ok = LoadJsonFile(p, j) && j.is_object();
    RemoveFile(p);
    r = Request();
    if (!ok) return true;  // (still: show the window)
    if (j.contains("paths") && j["paths"].is_array())
        for (auto& v : j["paths"])
            if (v.is_string()) r.paths.push_back(v.get<std::string>());
    r.send = j.value("send", false);
    return true;
}

}  // namespace instance
