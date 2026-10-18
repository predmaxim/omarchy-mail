#include "Theme.h"
#include <QFile>
#include <QGuiApplication>
#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <cstdio>

using namespace om;

static void write(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(data);
}

class TstTheme : public QObject {
    Q_OBJECT
    QString state, config;
    QByteArray oldPath;
    QTemporaryDir tmp;
private Q_SLOTS:
    void init()
    {
        // fresh isolated XDG dirs and PATH per test
        QVERIFY(tmp.isValid());
        QDir(tmp.path()).removeRecursively();
        QDir().mkpath(tmp.path());
        state = tmp.filePath(QStringLiteral("state"));
        config = tmp.filePath(QStringLiteral("config"));
        qputenv("XDG_STATE_HOME", state.toUtf8());
        qputenv("XDG_CONFIG_HOME", config.toUtf8());
        QDir().mkpath(tmp.filePath(QStringLiteral("emptybin")));
        oldPath = qgetenv("PATH");
        qputenv("PATH", tmp.filePath(QStringLiteral("emptybin")).toUtf8());
    }
    void cleanup() { qputenv("PATH", oldPath); }

    void parsePalette()
    {
        const auto p = Theme::parsePalette("background\t#101318\njunk\nfoo\tbar\n\naccent\t#112233\nx\t#12\n");
        QCOMPARE(p.size(), 2);
        QCOMPARE(p.value("background"), QColor("#101318"));
        QCOMPARE(p.value("accent"), QColor("#112233"));
    }
    void tomlMuted()
    {
        const auto p = Theme::parseColorsToml("# c\n[colors]\nbackground = \"#000000\"\ncolor8 = \"#222222\"\nmuted = \"#333333\"\nforeground=\"#ffffff\"\n");
        QCOMPARE(p.value("muted"), QColor("#333333"));
        QCOMPARE(p.value("background"), QColor("#000000"));
    }
    void tomlMutedFromColor8()
    {
        QCOMPARE(Theme::parseColorsToml("color8 = \"#222222\"\nforeground = \"#ffffff\"\n").value("muted"), QColor("#222222"));
    }
    void tomlMutedFromForeground()
    {
        QCOMPARE(Theme::parseColorsToml("foreground = \"#ffffff\"\n").value("muted"), QColor("#ffffff"));
    }
    void baseSize()
    {
        QCOMPARE(Theme::parseBaseSize("[font]\nbase-size = 18\n", 16), 18);
        QCOMPARE(Theme::parseBaseSize("[other]\nbase-size = 30\n[font]\nfamily = \"x\"\n", 16), 16);
        QCOMPARE(Theme::parseBaseSize("[x]\nbase-size = 30\n", 14), 14);
        QCOMPARE(Theme::parseBaseSize("", 13), 13);
    }
    void scriptPalette()
    {
        const QString bin = tmp.filePath(QStringLiteral("bin"));
        write(bin + "/omarchy-theme-color", "#!/bin/sh\nprintf 'background\\t#101318\\nforeground\\t#eeeeee\\naccent\\t#abcdef\\n'\n");
        QFile::setPermissions(bin + "/omarchy-theme-color", QFile::permissions(bin + "/omarchy-theme-color") | QFileDevice::ExeOwner);
        qputenv("PATH", bin.toUtf8());
        Theme t;
        QCOMPARE(t.background(), QColor("#101318"));
        QCOMPARE(t.accent(), QColor("#abcdef"));
        QCOMPARE(t.muted(), QColor("#eeeeee"));   // ensured: no muted/color8 -> foreground
    }
    void scriptWithoutThemeFallsBack()
    {
        // script exits 0 but prints no background/foreground
        const QString bin = tmp.filePath(QStringLiteral("bin"));
        write(bin + "/omarchy-theme-color", "#!/bin/sh\nprintf 'accent\\t#333333\\n'\n");
        QFile::setPermissions(bin + "/omarchy-theme-color", QFile::permissions(bin + "/omarchy-theme-color") | QFileDevice::ExeOwner);
        qputenv("PATH", bin.toUtf8());
        Theme t;
        QCOMPARE(t.background(), QColor("#1e1e1e"));
    }
    void tomlFallback()
    {
        write(state + "/omarchy/current/theme/colors.toml", "background = \"#0a0b0c\"\nforeground = \"#fafafa\"\naccent = \"#123456\"\n");
        Theme t;
        QCOMPARE(t.background(), QColor("#0a0b0c"));
        QCOMPARE(t.accent(), QColor("#123456"));
    }
    void defaults()
    {
        Theme t;
        QCOMPARE(t.background(), QColor("#1e1e1e"));
        QCOMPARE(t.foreground(), QColor("#e6e6e6"));
        QCOMPARE(t.accent(), QColor("#8999aa"));
        QCOMPARE(t.muted(), QColor("#5c6370"));
        QCOMPARE(t.baseSize(), 16);
        QCOMPARE(t.fontCaption(), 13);
        QCOMPARE(t.fontTitle(), 19);
        QCOMPARE(t.iconTheme(), QStringLiteral("Adwaita"));
    }
    void tokens_from_base_size()
    {
        write(config + "/omarchy/shell.toml", "[font]\nbase-size = 16\n");
        Theme t;
        QCOMPARE(t.fontCaption(), 13);
        QCOMPARE(t.fontBody(), 16);
        QCOMPARE(t.fontSubtitle(), 17);
        QCOMPARE(t.fontTitle(), 19);
        QCOMPARE(t.fontHeading(), 21);
        QCOMPARE(t.fontDisplay(), 32);
        QCOMPARE(t.iconLarge(), 24);
        QCOMPARE(t.space(3), 4);
        QCOMPARE(t.space(12), 16);
        QCOMPARE(t.space(18), 24);
        write(config + "/omarchy/shell.toml", "[font]\nbase-size = 12\n");
        Theme u;
        QCOMPARE(u.fontTitle(), 14);
        QCOMPARE(u.space(12), 12);
    }
    void urgent_fallbacks()
    {
        const QString bin = tmp.filePath(QStringLiteral("bin"));
        auto withPalette = [&](const char *body) {
            write(bin + "/omarchy-theme-color", QByteArray("#!/bin/sh\nprintf '") + body + "'\n");
            QFile::setPermissions(bin + "/omarchy-theme-color", QFile::permissions(bin + "/omarchy-theme-color") | QFileDevice::ExeOwner);
            qputenv("PATH", bin.toUtf8());
        };
        const char *base = "background\\t#101318\\nforeground\\t#eeeeee\\naccent\\t#abcdef\\n";
        withPalette((QByteArray(base) + "red\\t#ff0000\\ncolor1\\t#00ff00\\n").constData());
        QCOMPARE(Theme().urgent(), QColor("#ff0000"));
        withPalette((QByteArray(base) + "color1\\t#00ff00\\n").constData());
        QCOMPARE(Theme().urgent(), QColor("#00ff00"));
        withPalette(base);
        Theme t;
        QCOMPARE(t.urgent(), QColor("#a55555"));
        QCOMPARE(t.mark(), QColor("#abcdef"));        // no yellow/color3 -> accent
        QCOMPARE(t.selection(), QColor("#abcdef"));   // no selection -> accent
        withPalette((QByteArray(base) + "selection\\t#2b3040\\n").constData());
        QCOMPARE(Theme().selection().name(), QString("#58677d"));   // a third of the way to accent: brighter
    }
    void surface_is_a_step_toward_foreground()
    {
        const QString bin = tmp.filePath(QStringLiteral("bin"));
        auto withPalette = [&](const char *body) {
            write(bin + "/omarchy-theme-color", QByteArray("#!/bin/sh\nprintf '") + body + "'\n");
            QFile::setPermissions(bin + "/omarchy-theme-color", QFile::permissions(bin + "/omarchy-theme-color") | QFileDevice::ExeOwner);
            qputenv("PATH", bin.toUtf8());
        };
        withPalette("background\\t#000000\\nforeground\\t#ffffff\\naccent\\t#abcdef\\n");   // dark: lighter, 3 % of 255
        QColor dark = Theme().surface();
        QVERIFY(qAbs(dark.red() - 8) <= 1 && dark.red() == dark.green() && dark.green() == dark.blue());
        withPalette("background\\t#ffffff\\nforeground\\t#000000\\naccent\\t#abcdef\\n");   // light: darker
        QColor light = Theme().surface();
        QVERIFY(qAbs(light.red() - 247) <= 1 && light.red() == light.blue());
    }
    void fills_from_foreground()
    {
        Theme t;
        QVERIFY(qAbs(t.currentFill().alphaF() - 0.18) < 0.005);
        QCOMPARE(t.currentFill().rgb(), t.foreground().rgb());
        QVERIFY(qAbs(t.hoverFill().alphaF() - 0.08) < 0.005);
        QVERIFY(qAbs(t.cursorBorder().alphaF() - 0.25) < 0.005);
        QCOMPARE(t.dim(), t.foreground().darker(140));
    }
    void iconTheme()
    {
        write(state + "/omarchy/current/theme/icons.theme", "  Yaru-blue\n");
        Theme t;
        QCOMPARE(t.iconTheme(), QStringLiteral("Yaru-blue"));
    }
    void userOverridesThemeBaseSize()
    {
        write(state + "/omarchy/current/theme/shell.toml", "[font]\nbase-size = 12\n");
        QCOMPARE(Theme().baseSize(), 12);
        write(config + "/omarchy/shell.toml", "[font]\nbase-size = 20\n");
        Theme t;
        QCOMPARE(t.baseSize(), 20);
        QCOMPARE(t.fontCaption(), 17);
        QCOMPARE(t.fontTitle(), 23);
    }
    void watchThemeName()
    {
        write(state + "/omarchy/current/theme.name", "a\n");
        Theme t;
        QSignalSpy spy(&t, &Theme::changed);
        write(state + "/omarchy/current/theme.name", "b\n");
        QVERIFY(spy.wait(3000));
    }
    void watchAtomicReplaceTwice()
    {
        const QString f = config + "/omarchy/shell.toml";
        write(f, "[font]\nbase-size = 15\n");
        Theme t;
        QCOMPARE(t.baseSize(), 15);
        QSignalSpy spy(&t, &Theme::changed);
        for (int size : {21, 22}) {
            write(f + ".new", QByteArray("[font]\nbase-size = ") + QByteArray::number(size) + "\n");
            QCOMPARE(std::rename((f + ".new").toUtf8().constData(), f.toUtf8().constData()), 0);
            QVERIFY(spy.wait(3000));
            QTRY_COMPARE(t.baseSize(), size);
        }
    }
    void shellTomlCreatedLater()
    {
        QDir().mkpath(config + "/omarchy");   // the folder watch needs the folder
        Theme t;
        QSignalSpy spy(&t, &Theme::changed);
        write(config + "/omarchy/shell.toml", "[font]\nbase-size = 19\n");
        QVERIFY(spy.wait(3000));
        QTRY_COMPARE(t.baseSize(), 19);
    }
};

QTEST_MAIN(TstTheme)
#include "tst_theme.moc"
