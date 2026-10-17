#include "Session.h"
#include "LetterView.h"
#include <QFont>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <utility>
#include <cstdio>

namespace om {

Session::Session(const QString &socketPath, const QString &draftsDir, const QString &imageDir)
    : client(socketPath), images(imageDir), letter(&client, &images, &renderer, &theme), compose(draftsDir),
      app(&client, &list, &letter, &compose, &nav, &theme)
{
}

void Session::load(QQmlApplicationEngine &engine)
{
    static const int registered = qmlRegisterType<LetterView>("OmMail", 1, 0, "LetterView");
    Q_UNUSED(registered)
    QGuiApplication::setFont(QFont(QStringLiteral("monospace")));   // every Text: fontconfig gives Omarchy's font
    QQmlContext *c = engine.rootContext();
    c->setContextProperty(QStringLiteral("theme"), &theme);
    c->setContextProperty(QStringLiteral("i18n"), &i18n);
    c->setContextProperty(QStringLiteral("app"), &app);
    c->setContextProperty(QStringLiteral("list"), &list);
    c->setContextProperty(QStringLiteral("letter"), &letter);
    c->setContextProperty(QStringLiteral("compose"), &compose);
    c->setContextProperty(QStringLiteral("nav"), &nav);
    // theme.space() is a call, so QML can't see it depend on the base size: a new base size sets `theme` again,
    // and every binding that reads it is evaluated anew (the font tokens are properties and follow by themselves)
    QObject::connect(&theme, &Theme::changed, &engine, [this, c, base = theme.baseSize()]() mutable {
        if (std::exchange(base, theme.baseSize()) != base)
            c->setContextProperty(QStringLiteral("theme"), &theme);
    });
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
}

Session::Args Session::parseArgs(const QStringList &args)
{
    Args out;
    for (qsizetype i = 0; i < args.size(); ++i) {
        const QString &a = args.at(i);
        if (a == QLatin1String("--open") || a.startsWith(QLatin1String("--open="))) {
            const QString v = a.size() > 6 ? a.mid(7) : (i + 1 < args.size() ? args.at(++i) : QString());
            bool ok = false;
            const qint64 id = v.toLongLong(&ok);
            if (ok)
                out.openId = id;
            else
                std::fprintf(stderr, "omarchy-mail: --open wants a letter id, not \"%s\"\n", qPrintable(v));
        } else if (a.startsWith(QLatin1String("mailto:"), Qt::CaseInsensitive)) {
            out.mailto << QUrl(a);
        }
    }
    return out;
}

void Session::handleArgs(const Args &args, bool forwarded)
{
    if (forwarded && args.openId != 0)
        app.openMessage(args.openId);
    for (const QUrl &u : args.mailto)
        app.openMailto(u);
    if (forwarded)
        Q_EMIT app.raiseRequested();
}

void Session::start(qint64 openId)
{
    client.start();
    app.start(openId);
    images.prune();
}

} // namespace om
