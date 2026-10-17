#pragma once
// Test helpers: a stand-in for uwsm-app that records its argv, and scratch dirs inside the build dir.
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QStringList>

namespace om::test {

// Template for QTemporaryDir under the test cache (the build dir), never /tmp or the user's folders.
inline QString scratch()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/omwork";
    QDir().mkpath(dir);
    return dir + "/tst-XXXXXX";
}

// Writes <dir>/uwsm-app: each run appends its argv, '|'-separated, as one line of <dir>/uwsm-app.log.
inline QString writeStub(const QString &dir)
{
    const QString path = dir + "/uwsm-app";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return {};
    f.write("#!/bin/sh\n{ printf '%s|' \"$@\"; echo; } >> \"$0.log\"\n");
    f.close();
    f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

inline QList<QStringList> stubCalls(const QString &stub)
{
    QFile f(stub + ".log");
    QList<QStringList> out;
    if (!f.open(QIODevice::ReadOnly))
        return out;
    for (const QByteArray &line : f.readAll().split('\n'))
        if (!line.isEmpty())
            out << QString::fromUtf8(line.chopped(1)).split('|');
    return out;
}

} // namespace om::test
