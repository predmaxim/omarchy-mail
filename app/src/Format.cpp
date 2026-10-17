#include "Format.h"
#include "I18n.h"
#include <QJsonObject>

namespace om::format {

namespace {
const QString kFull = QStringLiteral("d MMM yyyy, HH:mm");
}

QString listDate(qint64 ts, const QDateTime &now, const QLocale &time)
{
    if (ts == 0)
        return {};
    const QDateTime dt = QDateTime::fromSecsSinceEpoch(ts);
    const qint64 days = dt.date().daysTo(now.date());
    if (days == 0)
        return time.toString(dt.time(), QStringLiteral("HH:mm"));
    if (days == 1)
        return t("yesterday");
    if (days > 1 && days < 7)
        return time.dayName(dt.date().dayOfWeek(), QLocale::ShortFormat);
    if (dt.date().year() == now.date().year())
        return time.toString(dt.date(), QStringLiteral("d MMM"));
    return time.toString(dt.date(), QLocale::ShortFormat);
}

QString letterDate(qint64 ts, const QDateTime &now, const QLocale &time)
{
    const QDateTime dt = QDateTime::fromSecsSinceEpoch(ts);
    if (dt.date() == now.date())
        return t("today") + QLatin1Char(' ') + time.toString(dt.time(), QStringLiteral("HH:mm"));
    return quoteDate(ts, time);
}

QString quoteDate(qint64 ts, const QLocale &time)
{
    return time.toString(QDateTime::fromSecsSinceEpoch(ts), kFull);
}

QString size(qint64 bytes)
{
    static const char *const units[] = {"B", "KB", "MB", "GB"};
    double v = double(bytes);
    int u = 0;
    while (v >= 1024 && u < 3) {
        v /= 1024;
        ++u;
    }
    const QLocale loc(I18n::ru() ? QStringLiteral("ru_RU") : QStringLiteral("en_US"));
    const QString num = (u == 0 || v >= 9.95) ? QString::number(qlonglong(v + 0.5)) : loc.toString(v, 'f', 1);
    return num + QLatin1Char(' ') + t(units[u]);
}

QString folderLabel(const QString &role, const QString &name)
{
    if (role == QLatin1String("inbox"))
        return t("Inbox");
    if (role == QLatin1String("flagged"))
        return t("Flagged");
    if (role == QLatin1String("sent"))
        return t("Sent");
    if (role == QLatin1String("trash"))
        return t("Trash");
    if (role == QLatin1String("junk"))
        return t("Junk");
    return name;
}

StatusLine statusLine(bool connected, const QJsonArray &accounts)
{
    if (!connected)
        return {t("Mail service is not running"), true};
    QStringList parts, offline;
    int queued = 0;
    for (const auto &v : accounts) {
        const QJsonObject a = v.toObject();
        const QString email = a.value(QLatin1String("email")).toString();
        queued += a.value(QLatin1String("queued")).toInt();
        const QJsonValue st = a.value(QLatin1String("status"));
        if (st.isObject())
            offline << email;
        else if (st.toString() == QLatin1String("auth_failed"))
            parts << t("%1: login rejected").arg(email);
    }
    const bool error = !parts.isEmpty();
    if (!offline.isEmpty()) {
        if (offline.size() == accounts.size())
            parts << t("offline");
        else if (offline.size() == 1)
            parts << t("%1 offline").arg(offline.first());
        else
            parts << tn("%n mailbox offline|%n mailboxes offline", int(offline.size()));
    }
    if (queued > 0)
        parts << tn("%n queued", queued);
    return {parts.join(QStringLiteral(" · ")), error};
}

} // namespace om::format
