#include "I18n.h"
#include <QFile>

namespace om {

namespace {

int g_ru = -1;   // -1: not decided yet, take from the environment

QString firstEnv(std::initializer_list<const char *> vars)
{
    for (const char *v : vars) {
        const QString s = qEnvironmentVariable(v);
        if (!s.isEmpty())
            return s;
    }
    return {};
}

const QHash<QString, QString> &table()
{
    static const QHash<QString, QString> tbl = [] {
        QFile f(QStringLiteral(":/i18n/ru.tsv"));
        return f.open(QIODevice::ReadOnly) ? I18n::loadTable(f.readAll()) : QHash<QString, QString>();
    }();
    return tbl;
}

QString translate(const QString &en)
{
    return I18n::ru() ? table().value(en, en) : en;
}

QString plural(const QString &en, int n)
{
    const QStringList forms = translate(en).split(QLatin1Char('|'));
    int i = 0;
    if (I18n::ru() && forms.size() >= 3) {
        const int m10 = n % 10, m100 = n % 100;
        i = (m10 == 1 && m100 != 11) ? 0 : (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) ? 1 : 2;
    } else {
        i = n == 1 ? 0 : 1;
    }
    QString form = forms.at(qMin(i, int(forms.size()) - 1));
    return form.replace(QStringLiteral("%n"), QString::number(n));
}

} // namespace

bool I18n::ruFromEnv() { return firstEnv({"LC_ALL", "LC_MESSAGES", "LANG"}).startsWith(QLatin1String("ru")); }
void I18n::setRu(bool ru) { g_ru = ru; }
bool I18n::ru()
{
    if (g_ru < 0)
        g_ru = ruFromEnv();
    return g_ru;
}

QHash<QString, QString> I18n::loadTable(const QByteArray &tsv)
{
    QHash<QString, QString> out;
    for (const QByteArray &line : tsv.split('\n')) {
        const int tab = line.indexOf('\t');
        if (line.startsWith('#') || tab <= 0)
            continue;
        out.insert(QString::fromUtf8(line.left(tab)), QString::fromUtf8(line.mid(tab + 1)).trimmed());
    }
    return out;
}

QString I18n::t(const QString &en) const { return translate(en); }
QString I18n::tn(const QString &en, int n) const { return plural(en, n); }

QString t(const char *en) { return translate(QString::fromUtf8(en)); }
QString tn(const char *en, int n) { return plural(QString::fromUtf8(en), n); }

QLocale timeLocale()
{
    QString s = firstEnv({"LC_ALL", "LC_TIME", "LANG"});
    if (s.isEmpty())
        return QLocale::system();
    s = s.section(QLatin1Char('.'), 0, 0);
    return QLocale(s);
}

} // namespace om
