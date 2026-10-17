// copied from omarchy-quicklook (own MIT code)
#include "Theme.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QProcess>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace om {

namespace {

const QHash<QString, QColor> &defaults()
{
    static const QHash<QString, QColor> d = {
        {QStringLiteral("background"), QColor("#1e1e1e")}, {QStringLiteral("foreground"), QColor("#e6e6e6")},
        {QStringLiteral("accent"), QColor("#8999aa")},     {QStringLiteral("muted"), QColor("#5c6370")},
        {QStringLiteral("color8"), QColor("#5c6370")},     {QStringLiteral("magenta"), QColor("#c678dd")},
        {QStringLiteral("blue"), QColor("#61afef")},       {QStringLiteral("green"), QColor("#98c379")},
        {QStringLiteral("yellow"), QColor("#e5c07b")},     {QStringLiteral("orange"), QColor("#d19a66")},
        {QStringLiteral("cyan"), QColor("#56b6c2")},       {QStringLiteral("red"), QColor("#e06c75")},
    };
    return d;
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QString xdgDir(const char *var, const QString &homeRel)
{
    const QString v = qEnvironmentVariable(var);
    return (v.isEmpty() ? QDir::homePath() + homeRel : v) + QStringLiteral("/omarchy");
}

void ensureMuted(QHash<QString, QColor> &p)
{
    if (p.contains(QStringLiteral("muted")))
        return;
    if (p.contains(QStringLiteral("color8")))
        p.insert(QStringLiteral("muted"), p.value(QStringLiteral("color8")));
    else if (p.contains(QStringLiteral("foreground")))
        p.insert(QStringLiteral("muted"), p.value(QStringLiteral("foreground")));
}

bool usable(const QHash<QString, QColor> &p)
{
    return p.contains(QStringLiteral("background")) && p.contains(QStringLiteral("foreground"));
}

} // namespace

Theme::Theme(QObject *parent) : QObject(parent)
{
    auto *settle = new QTimer(this);
    settle->setSingleShot(true);
    settle->setInterval(100);
    connect(settle, &QTimer::timeout, this, [this] {
        reload();   // emits changed()
        rewatch();
    });
    const auto trigger = [settle] { settle->start(); };
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, trigger);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, trigger);
    reload();
    rewatch();
}

QString Theme::currentDir() { return xdgDir("XDG_STATE_HOME", QStringLiteral("/.local/state")) + QStringLiteral("/current"); }
QString Theme::userShellToml() { return xdgDir("XDG_CONFIG_HOME", QStringLiteral("/.config")) + QStringLiteral("/shell.toml"); }

void Theme::rewatch()
{
    const QStringList old = m_watcher.files() + m_watcher.directories();
    if (!old.isEmpty())
        m_watcher.removePaths(old);   // a replaced inode keeps a dead watch otherwise
    m_watcher.addPaths({currentDir() + QStringLiteral("/theme.name"), userShellToml(), QFileInfo(userShellToml()).path()});
}

void Theme::reload()
{
    const QString colors = currentDir() + QStringLiteral("/theme/colors.toml");
    QHash<QString, QColor> pal;
    QProcess proc;
    proc.start(QStringLiteral("omarchy-theme-color"), {QStringLiteral("--file"), colors, QStringLiteral("--all")});
    if (proc.waitForFinished(2000) && proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0)
        pal = parsePalette(proc.readAllStandardOutput());
    else if (proc.state() != QProcess::NotRunning) {
        proc.kill();
        proc.waitForFinished(500);
    }
    if (!usable(pal))
        pal = parseColorsToml(readFile(colors));
    if (!usable(pal))
        pal = defaults();
    ensureMuted(pal);
    m_palette = pal;

    int base = parseBaseSize(readFile(currentDir() + QStringLiteral("/theme/shell.toml")), 16);
    m_baseSize = parseBaseSize(readFile(userShellToml()), base);

    m_iconTheme = QString::fromUtf8(readFile(currentDir() + QStringLiteral("/theme/icons.theme"))).trimmed();
    if (m_iconTheme.isEmpty())
        m_iconTheme = QStringLiteral("Adwaita");
    if (qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
        QIcon::setThemeName(m_iconTheme);
    Q_EMIT changed();
}

QColor Theme::background() const { return m_palette.value(QStringLiteral("background"), defaults().value(QStringLiteral("background"))); }
QColor Theme::foreground() const { return m_palette.value(QStringLiteral("foreground"), defaults().value(QStringLiteral("foreground"))); }
QColor Theme::accent() const { return m_palette.value(QStringLiteral("accent"), defaults().value(QStringLiteral("accent"))); }
QColor Theme::muted() const { return m_palette.value(QStringLiteral("muted"), defaults().value(QStringLiteral("muted"))); }
QColor Theme::urgent() const
{
    for (const char *k : {"red", "color1"})
        if (m_palette.contains(QLatin1String(k)))
            return m_palette.value(QLatin1String(k));
    return QColor("#a55555");
}
QColor Theme::dim() const { return foreground().darker(140); }
QColor Theme::mark() const
{
    for (const char *k : {"yellow", "color3"})
        if (m_palette.contains(QLatin1String(k)))
            return m_palette.value(QLatin1String(k));
    return accent();
}
QColor Theme::selection() const { return m_palette.value(QStringLiteral("selection"), accent()); }
QColor Theme::fill(double alpha) const
{
    QColor c = foreground();
    c.setAlphaF(alpha);
    return c;
}
QColor Theme::hoverFill() const { return fill(0.08); }
QColor Theme::currentFill() const { return fill(0.18); }
QColor Theme::cursorBorder() const { return fill(0.25); }
int Theme::scaled(double f) const { return std::max(1, int(std::lround(m_baseSize * f))); }
int Theme::space(int px) const { return std::max(1, int(std::lround(px * m_baseSize / 12.0))); }

QHash<QString, QColor> Theme::parsePalette(const QByteArray &tsv)
{
    QHash<QString, QColor> out;
    for (const QByteArray &line : tsv.split('\n')) {
        const int tab = line.indexOf('\t');
        if (tab <= 0)
            continue;
        const QColor c(QString::fromUtf8(line.mid(tab + 1).trimmed()));
        if (c.isValid())
            out.insert(QString::fromUtf8(line.left(tab).trimmed()), c);
    }
    return out;
}

QHash<QString, QColor> Theme::parseColorsToml(const QByteArray &toml)
{
    QHash<QString, QColor> out;
    for (const QByteArray &raw : toml.split('\n')) {
        const QByteArray line = raw.trimmed();
        const int eq = line.indexOf('=');
        if (line.startsWith('#') || line.startsWith('[') || eq <= 0)
            continue;
        QByteArray v = line.mid(eq + 1).trimmed();
        if (v.size() < 2 || !v.startsWith('"') || v.indexOf('"', 1) < 0)
            continue;
        const QColor c(QString::fromUtf8(v.mid(1, v.indexOf('"', 1) - 1)));
        if (c.isValid())
            out.insert(QString::fromUtf8(line.left(eq).trimmed()), c);
    }
    ensureMuted(out);
    return out;
}

int Theme::parseBaseSize(const QByteArray &toml, int fallback)
{
    bool inFont = false;
    for (const QByteArray &raw : toml.split('\n')) {
        const QByteArray line = raw.trimmed();
        if (line.startsWith('[')) {
            inFont = line.startsWith("[font]");
            continue;
        }
        const int eq = line.indexOf('=');
        if (!inFont || eq <= 0 || line.left(eq).trimmed() != "base-size")
            continue;
        bool ok = false;
        const int n = line.mid(eq + 1).trimmed().toInt(&ok);
        if (ok && n > 0)
            return n;
    }
    return fallback;
}

} // namespace om
