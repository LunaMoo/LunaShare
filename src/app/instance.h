// LunaShare - one copy per settings folder (desktops). A second start (a file opened with LunaShare from the file
// manager, or the program started again) hands its files to the running copy and ends: it drops a small request
// file into <config>/requests, which the running copy picks up a few times a second.
#pragma once
#include <string>
#include <vector>

namespace instance {

// true = this is the only copy for that folder (it keeps the lock until it ends)
bool Acquire(const std::string& configDir);

// the second copy: hand over (paths made absolute; none = only show the window)
void SendRequest(const std::string& configDir, const std::vector<std::string>& paths, bool send);

// the running copy: a request that came ("show" = bring the window up; paths may be empty)
struct Request {
    std::vector<std::string> paths;
    bool send = false;  // share them (to a chat), not open them
};
bool Poll(const std::string& configDir, Request& r);
// at the start of the copy that runs: requests left from a copy that ended before it took them are dropped
void ClearRequests(const std::string& configDir);

}  // namespace instance
