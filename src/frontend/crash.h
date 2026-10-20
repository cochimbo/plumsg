#pragma once

#include <string>

namespace plumsg {

// When plumsg dies of a crash (an access violation, an abort, an uncaught exception),
// writes a report to dir: plumsg-AAAAMMDD-HHMMSS.txt with what happened, where and with
// which game, and on Windows a minidump (.dmp) next to it to open in a debugger. Then
// it lets the system end the program as it would have.
void installCrashHandler(const std::string& dir);

// What the report says plumsg was doing: the game that is loaded ("" for none).
void setCrashContext(const std::string& game);

// The report the last run left behind and nobody has been told about yet, or "" if it
// ended well. Marks it as told.
std::string takeCrashReport(const std::string& dir);

}  // namespace plumsg
