#include "oplog.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>

#include <csignal>
#include <cstring>
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr int kRing = 64, kLine = 240;
char g_ring[kRing][kLine];  // recent operations, readable from the signal handler
int g_ringPos = 0, g_ringCount = 0;
char g_crashPath[2048] = {0};
QFile* g_session = nullptr;
QtMessageHandler g_prevHandler = nullptr;

void put(int fd, const char* s)
{
    if (fd >= 0) (void)!::write(fd, s, std::strlen(s));
}

const char* signalName(int sig)
{
    switch (sig) {
    case SIGSEGV: return "SIGSEGV (invalid memory access)";
    case SIGABRT: return "SIGABRT (abort / assertion / uncaught exception)";
    case SIGFPE: return "SIGFPE (arithmetic error)";
    case SIGILL: return "SIGILL (illegal instruction)";
    case SIGBUS: return "SIGBUS (bus error)";
    default: return "unknown signal";
    }
}

// Only async-signal-safe calls (write, backtrace_symbols_fd, …) in here.
void crashHandler(int sig)
{
    const int fd = ::open(g_crashPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
    for (int out : {fd, 2}) {
        put(out, "\n===== TooD crashed: ");
        put(out, signalName(sig));
        put(out, " =====\nBacktrace (resolve '+0x…' offsets with: addr2line -f -C -e build/tood 0x…):\n");
        void* frames[64];
        const int n = backtrace(frames, 64);
        if (out >= 0) backtrace_symbols_fd(frames, n, out);
        put(out, "Last operations (oldest first):\n");
        const int start = (g_ringPos - g_ringCount + kRing) % kRing;
        for (int i = 0; i < g_ringCount; ++i) {
            put(out, "  ");
            put(out, g_ring[(start + i) % kRing]);
            put(out, "\n");
        }
        put(out, "=====\n");
    }
    if (fd >= 0) ::close(fd);
    std::signal(sig, SIG_DFL);  // let the default action run (core dump if enabled)
    std::raise(sig);
}

void messageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    if (type != QtDebugMsg) {
        static const char* names[] = {"debug", "warning", "critical", "fatal", "info"};
        OpLog::op(QStringLiteral("qt-%1: %2").arg(QString::fromLatin1(names[qBound(0, int(type), 4)]), msg));
    }
    if (g_prevHandler) g_prevHandler(type, ctx, msg);
}

QString dataDir()
{
    const QString d = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(d);
    return d;
}

}  // namespace

namespace OpLog {

void installCrashHandler()
{
    const QByteArray p = QFile::encodeName(crashLogPath());
    qstrncpy(g_crashPath, p.constData(), sizeof(g_crashPath));
    void* warm[2];
    backtrace(warm, 2);  // loads libgcc now, so backtrace() does not allocate inside the handler
    for (int sig : {SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS}) std::signal(sig, crashHandler);
}

bool startSession(const QString& pathIn)
{
    const QString path = pathIn.isEmpty()
                             ? QDir(dataDir()).filePath(QStringLiteral("session-%1.log")
                                                            .arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss")))
                             : pathIn;
    auto* f = new QFile(path);
    if (!f->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        delete f;
        return false;
    }
    g_session = f;
    g_prevHandler = qInstallMessageHandler(messageHandler);
    return true;
}

void op(const QString& text)
{
    const QString line = QTime::currentTime().toString("HH:mm:ss.zzz  ") + text;
    const QByteArray u = line.toUtf8();
    qstrncpy(g_ring[g_ringPos], u.constData(), kLine);
    g_ringPos = (g_ringPos + 1) % kRing;
    g_ringCount = qMin(g_ringCount + 1, kRing);
    if (g_session) {
        g_session->write(u);
        g_session->write("\n");
        g_session->flush();  // complete even if we crash right after
    }
}

QString crashLogPath() { return QDir(dataDir()).filePath(QStringLiteral("crash.log")); }

QString sessionLogPath() { return g_session ? g_session->fileName() : QString(); }

}  // namespace OpLog
