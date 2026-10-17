#pragma once
// UI language (en/ru) and date locale. Strings are English in code, translated via i18n/ru.tsv.
#include <QHash>
#include <QLocale>
#include <QObject>

namespace om {

QString t(const char *en);
QString tn(const char *en, int n);   // en: "%n message|%n messages"; ru table: three forms
QLocale timeLocale();                // first non-empty of LC_ALL, LC_TIME, LANG (without .UTF-8)

class I18n : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    static bool ruFromEnv();   // first non-empty of LC_ALL, LC_MESSAGES, LANG starts with "ru"
    static void setRu(bool ru);
    static bool ru();
    static QHash<QString, QString> loadTable(const QByteArray &tsv);   // "en<TAB>ru" lines, # comments

    Q_INVOKABLE QString t(const QString &en) const;
    Q_INVOKABLE QString tn(const QString &en, int n) const;
};

} // namespace om
