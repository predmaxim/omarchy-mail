// Skeleton: the window arrives in a later task.
#include "Launch.h"
#include <QGuiApplication>
#include <QProcessEnvironment>

static QProcessEnvironment launchEnv;   // for child processes (setProcessEnvironment)

int main(int argc, char *argv[])
{
    launchEnv = QProcessEnvironment::systemEnvironment();
    qputenv("QT_QPA_PLATFORMTHEME", "xdgdesktopportal");
    om::launch::setEnvironment(launchEnv);
    QGuiApplication app(argc, argv);
    QGuiApplication::setDesktopFileName(QStringLiteral("omarchy-mail"));
    QString openId;
    const QStringList args = app.arguments();
    for (int i = 1; i + 1 < args.size(); ++i)
        if (args.at(i) == QLatin1String("--open"))
            openId = args.at(i + 1);
    Q_UNUSED(openId)
    return 0;
}
