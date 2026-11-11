#pragma once
// $XDG_CONFIG_HOME/omarchy-mail/window.json: the window's own settings, one file for all of them.
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace om::prefs {

QJsonObject read();   // {} when the file is missing or broken
// Read-modify-write through QSaveFile; the file is 0600, its folder 0700. A null value removes the key.
void write(const QString &key, const QJsonValue &value);

} // namespace om::prefs
