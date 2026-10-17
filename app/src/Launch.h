#pragma once
// Child programs (uwsm-app -- xdg-open), the previewer, attachment copies, originals for the browser.
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QString>

namespace om::launch {

void setEnvironment(const QProcessEnvironment &env);   // main(): the environment before our qputenv
void setProgramForTests(const QString &program);       // replaces uwsm-app
bool open(const QString &target);                      // [uwsm-app --] xdg-open <target>, detached
void preview(const QString &path);                     // NautilusPreviewer over D-Bus, else open()
qreal textScale();   // GNOME's text-scaling-factor (gsettings), 0.5–3; 1 without it
QString saveCopy(const QString &src, const QString &dir);   // name.ext, name (2).ext, …; "" on error
QString downloadsDir();
// <dir>/<summary.id>.html, 0600, CSP first; cid: images point at the files of full.inline. "" on error.
QString writeOriginal(const QJsonObject &full, const QString &dir);

} // namespace om::launch
