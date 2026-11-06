// Operation log + crash log.
//
// Every operation is kept in a small in-memory ring buffer (last 64). If the
// program crashes, a signal handler appends the backtrace and that ring to
// crash.log in the app data folder. With --log-session, every operation is also
// written (and flushed) to a session log file.
#pragma once

#include <QString>

namespace OpLog {

void installCrashHandler();                   // call once, after QApplication exists
bool startSession(const QString& path = {});   // empty path: app data folder, timestamped name
void op(const QString& text);                 // record one operation
QString crashLogPath();
QString sessionLogPath();                     // empty when session logging is off

}  // namespace OpLog
