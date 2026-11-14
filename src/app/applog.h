// LunaShare - test builds' diagnostic log (LUNASHARE_DIAG): what SDL_Log and the engine (stderr) say, kept in memory
// (copied from Settings - About) and written to lunashare-log.txt in the LunaShare folder (the downloads), so it can
// be taken from a phone to a PC and sent along with a problem. In ordinary builds all of this does nothing.
#pragma once

#include <string>

namespace applog {
void Init();              // (once; safe to call again)
void SetDir(const std::string& dir);  // where lunashare-log.txt goes (the LunaShare folder)
bool On();                // a test build: the log is kept
void Add(const std::string& line);
std::string Text();       // the recent lines, oldest first
}  // namespace applog
