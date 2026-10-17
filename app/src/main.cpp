#include "I18n.h"
#include "Launch.h"
#include "Session.h"
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QProcessEnvironment>
#include <QQmlApplicationEngine>
#include <cstdio>

int main(int argc, char *argv[])
{
    const QProcessEnvironment launchEnv = QProcessEnvironment::systemEnvironment();   // for child programs
    qputenv("QT_QPA_PLATFORMTHEME", "xdgdesktopportal");   // the file dialog: Omarchy's portal
    QGuiApplication app(argc, argv);
    QGuiApplication::setDesktopFileName(QStringLiteral("omarchy-mail"));   // Wayland app-id, Hyprland's class
    om::I18n::setRu(om::I18n::ruFromEnv());
    om::launch::setEnvironment(launchEnv);

    QCommandLineParser args;
    const QCommandLineOption open(QStringLiteral("open"), QStringLiteral("Show this letter first."), QStringLiteral("id"));
    args.addOption(open);
    args.process(app);
    qint64 openId = 0;
    if (args.isSet(open)) {
        bool ok = false;
        openId = args.value(open).toLongLong(&ok);
        if (!ok)
            std::fprintf(stderr, "omarchy-mail: --open wants a letter id, not \"%s\"\n", qPrintable(args.value(open)));
    }

    om::Session session(om::Client::defaultSocketPath(), om::Compose::defaultDir(), om::Images::defaultDir());
    QQmlApplicationEngine engine;   // dies before the session
    session.load(engine);
    if (engine.rootObjects().isEmpty())
        return 1;
    session.start(openId);
    return app.exec();
}
