#include "Prefs.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

namespace om::prefs {

static QString file()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/omarchy-mail/window.json");
}

QJsonObject read()
{
    QFile f(file());
    return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject();
}

void write(const QString &key, const QJsonValue &value)
{
    QJsonObject o = read();
    if (value.isNull())
        o.remove(key);
    else
        o.insert(key, value);
    const QString path = file(), dir = QFileInfo(path).path();
    QDir().mkpath(dir);
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QSaveFile f(path);
    f.setDirectWriteFallback(false);
    if (f.open(QIODevice::WriteOnly)) {
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        f.write(QJsonDocument(o).toJson());
        f.commit();   // a setting that isn't saved still holds for this window
    }
}

} // namespace om::prefs
