#pragma once
#include <QDateTime>
#include <QJsonArray>
#include <QLocale>
#include <QString>

namespace om::format {

QString listDate(qint64 ts, const QDateTime &now, const QLocale &time);
QString letterDate(qint64 ts, const QDateTime &now, const QLocale &time);
QString quoteDate(qint64 ts, const QLocale &time);   // "d MMM yyyy, HH:mm"
QString size(qint64 bytes);
QString folderLabel(const QString &role, const QString &name);

struct StatusLine {
    QString text;
    bool error = false;
};
// accounts: [{email, status, queued}] from the `status` reply
StatusLine statusLine(bool connected, const QJsonArray &accounts);

} // namespace om::format
