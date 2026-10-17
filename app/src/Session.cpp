#include "Session.h"
#include "LetterView.h"
#include <QFont>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <utility>

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

void Session::start(qint64 openId)
{
    client.start();
    app.start(openId);
    images.prune();
}

} // namespace om
