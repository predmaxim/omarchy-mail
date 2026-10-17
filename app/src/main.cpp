#include "I18n.h"
#include "Launch.h"
#include "Session.h"
#include "Single.h"
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

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Omarchy mail window"));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("open"), QStringLiteral("Show this letter first."), QStringLiteral("id")});
    parser.addPositionalArgument(QStringLiteral("mailto"), QStringLiteral("A mailto: link: the new-message form."), QStringLiteral("[mailto:...]"));
    parser.process(app);   // --help / -h print and exit; a bad --open id is reported below, the window still opens

    const QStringList cmdline = app.arguments().mid(1);
    om::single::Server single;
    switch (single.claim(cmdline)) {   // before the window is built: two first launches cannot both pass
    case om::single::Server::Forwarded:
        return 0;   // the open window takes the arguments and comes forward
    case om::single::Server::Unreachable:
        std::fprintf(stderr, "omarchy-mail: a window holds the socket but does not answer\n");
        return 1;
    case om::single::Server::Listening:
        break;
    }
    om::Session *live = nullptr;
    QList<QStringList> early;   // forwarded before the window exists
    QObject::connect(&single, &om::single::Server::received, &app, [&](const QStringList &a) {
        if (live)
            live->handleArgs(om::Session::parseArgs(a), true);
        else
            early << a;
    });
    const om::Session::Args args = om::Session::parseArgs(cmdline);

    om::Session session(om::Client::defaultSocketPath(), om::Compose::defaultDir(), om::Images::defaultDir());
    QQmlApplicationEngine engine;   // dies before the session
    session.load(engine);
    if (engine.rootObjects().isEmpty())
        return 1;
    session.start(args.openId);
    session.handleArgs(args, false);
    live = &session;
    for (const QStringList &a : std::as_const(early))
        session.handleArgs(om::Session::parseArgs(a), true);
    return app.exec();
}
