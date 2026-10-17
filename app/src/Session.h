#pragma once
// The window's objects, in the order they live and die, and the QML that shows them (main and tst_ui).
#include "App.h"
#include "Client.h"
#include "Compose.h"
#include "I18n.h"
#include "Images.h"
#include "Letter.h"
#include "MailList.h"
#include "Nav.h"
#include "Renderer.h"
#include "Theme.h"

class QQmlApplicationEngine;

namespace om {

struct Session {
    Session(const QString &socketPath, const QString &draftsDir, const QString &imageDir);
    void load(QQmlApplicationEngine &engine);   // the engine must die first: QML holds these objects
    void start(qint64 openId);

    // The command line: `--open <id>` and `mailto:` URLs. A bad id is reported on stderr and ignored.
    struct Args {
        qint64 openId = 0;
        QList<QUrl> mailto;
        bool raise = true;   // an empty launch or a valid command; junk must not steal the focus
    };
    static Args parseArgs(const QStringList &args);
    // forwarded: from a later launch, to the window already open (the first launch's --open went to start()).
    void handleArgs(const Args &args, bool forwarded);

    Theme theme;
    I18n i18n;
    Client client;
    Images images;
    Renderer renderer;
    Letter letter;
    MailList list;
    Compose compose;
    Nav nav;
    App app;
};

} // namespace om
