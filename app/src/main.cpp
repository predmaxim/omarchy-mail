#include "I18n.h"
#include "Launch.h"
#include "Session.h"
#include "Single.h"
#include <QGuiApplication>
#include <QProcessEnvironment>
#include <QQmlApplicationEngine>

int main(int argc, char *argv[])
{
    const QProcessEnvironment launchEnv = QProcessEnvironment::systemEnvironment();   // for child programs
    qputenv("QT_QPA_PLATFORMTHEME", "xdgdesktopportal");   // the file dialog: Omarchy's portal
    QGuiApplication app(argc, argv);
    QGuiApplication::setDesktopFileName(QStringLiteral("omarchy-mail"));   // Wayland app-id, Hyprland's class
    om::I18n::setRu(om::I18n::ruFromEnv());
    om::launch::setEnvironment(launchEnv);

    const QStringList cmdline = app.arguments().mid(1);
    if (om::single::forward(cmdline))   // a window is open: it takes the arguments and comes forward
        return 0;
    const om::Session::Args args = om::Session::parseArgs(cmdline);

    om::Session session(om::Client::defaultSocketPath(), om::Compose::defaultDir(), om::Images::defaultDir());
    QQmlApplicationEngine engine;   // dies before the session
    session.load(engine);
    if (engine.rootObjects().isEmpty())
        return 1;
    session.start(args.openId);
    session.handleArgs(args, false);
    om::single::Server single;
    QObject::connect(&single, &om::single::Server::received, &app, [&session](const QStringList &a) {
        session.handleArgs(om::Session::parseArgs(a), true);
    });
    single.listen();   // ponytail: no socket, no forwarding: a second window opens (single.lua still guards it)
    return app.exec();
}
