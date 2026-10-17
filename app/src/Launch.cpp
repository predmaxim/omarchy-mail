#include "Launch.h"
#include "Letter.h"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

namespace om::launch {

namespace {
QProcessEnvironment g_env{QProcessEnvironment::InheritFromParent};
QString g_program;   // tests: a stand-in for uwsm-app
} // namespace

void setEnvironment(const QProcessEnvironment &env) { g_env = env; }
void setProgramForTests(const QString &program) { g_program = program; }

bool open(const QString &target)
{
    const QString uwsm = g_program.isEmpty() ? QStandardPaths::findExecutable(QStringLiteral("uwsm-app")) : g_program;
    QProcess p;
    p.setProcessEnvironment(g_env);
    if (uwsm.isEmpty()) {
        p.setProgram(QStringLiteral("xdg-open"));
        p.setArguments({target});
    } else {
        p.setProgram(uwsm);
        p.setArguments({QStringLiteral("--"), QStringLiteral("xdg-open"), target});
    }
    return p.startDetached();
}

void preview(const QString &path)
{
    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.gnome.NautilusPreviewer"), QStringLiteral("/org/gnome/NautilusPreviewer"),
        QStringLiteral("org.gnome.NautilusPreviewer2"), QStringLiteral("ShowFile"));
    call << QString::fromUtf8(QUrl::fromLocalFile(path).toEncoded()) << QString() << true << QString();
    const QDBusPendingCall pending = QDBusConnection::sessionBus().asyncCall(call);
    if (pending.isFinished()) {   // no session bus: failed at once, and a watcher would never fire
        if (pending.isError())
            open(path);
        return;
    }
    auto *watch = new QDBusPendingCallWatcher(pending);
    QObject::connect(watch, &QDBusPendingCallWatcher::finished, [path](QDBusPendingCallWatcher *w) {
        if (w->isError())
            open(path);
        w->deleteLater();
    });
}

QString saveCopy(const QString &src, const QString &dir)
{
    const QString name = QFileInfo(src).fileName();
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    const QString base = dot > 0 ? name.left(dot) : name, ext = dot > 0 ? name.mid(dot) : QString();
    QDir().mkpath(dir);
    for (int n = 1; n < 1000; ++n) {
        const QString dest = QDir(dir).filePath(
            n == 1 ? name : QStringLiteral("%1 (%2)%3").arg(base, QString::number(n), ext));
        if (QFile::copy(src, dest))
            return dest;
        if (!QFileInfo::exists(dest))
            return {};   // not a name clash: unreadable source, full disk, …
    }
    return {};
}

QString downloadsDir() { return QStandardPaths::writableLocation(QStandardPaths::DownloadLocation); }

QString writeOriginal(const QJsonObject &full, const QString &dir)
{
    static const QRegularExpression cid(QStringLiteral("cid:([^\"'\\s)>]+)"), QRegularExpression::CaseInsensitiveOption);
    const QJsonObject files = full.value(QStringLiteral("inline")).toObject();
    const QString body = renderSource(full, {}, {});
    QString html = QStringLiteral("<!doctype html><meta charset=\"utf-8\">"
                                  "<meta http-equiv=\"Content-Security-Policy\" content=\"script-src 'none'\">");
    qsizetype done = 0;
    for (auto it = cid.globalMatch(body); it.hasNext();) {
        const auto m = it.next();
        const QString file = files.value(QUrl::fromPercentEncoding(m.captured(1).toUtf8())).toString();
        if (file.isEmpty())
            continue;
        html += body.mid(done, m.capturedStart() - done) + QString::fromUtf8(QUrl::fromLocalFile(file).toEncoded());
        done = m.capturedEnd();
    }
    html += body.mid(done);

    const qint64 id = full.value(QStringLiteral("summary")).toObject().value(QStringLiteral("id")).toInteger();
    const QString path = QDir(dir).filePath(QString::number(id) + QStringLiteral(".html"));
    QDir().mkpath(dir);
    QFile f(path);
    const QByteArray data = html.toUtf8();
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate, QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || f.write(data) != data.size() || !f.flush())
        return {};
    return path;
}

} // namespace om::launch
