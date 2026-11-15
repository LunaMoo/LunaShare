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

// Crash reports (every build, not only test builds): a native crash (a signal) or an uncaught Java exception writes
// what happened - the faulting place and a backtrace - to lunashare-crash.txt in the LunaShare folder (SetDir), then
// lets the system handle the crash as before. Something to send along when the app "stopped".
void CrashReportsInit();                  // (once; after that SetDir also says where the crash file goes)
void CrashText(const std::string& text);  // appends to the crash file (the Java handler's report)
// the report of a crash since the last start that hasn't been offered to the user yet (empty: none) - the end of the
// crash file past what CrashReportSeen() last marked (lunashare-crash.seen beside it)
std::string NewCrashReport();
void CrashReportSeen();
}  // namespace applog
