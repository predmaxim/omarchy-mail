#pragma once
// The App with its models on a FakeMaild serving the fixtures (two mailboxes, 60 rows), for the App tests.
#include "App.h"
#include "Client.h"
#include "Compose.h"
#include "FakeMaild.h"
#include "Images.h"
#include "Launch.h"
#include "LaunchStub.h"
#include "Letter.h"
#include "MailList.h"
#include "Nav.h"
#include "Renderer.h"
#include "Theme.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>

namespace om::test {

inline QJsonDocument fixture(const char *name)
{
    QFile f(QStringLiteral(OM_SOURCE_DIR "/tests/fixtures/") + QLatin1String(name));
    return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()) : QJsonDocument();
}

// A request as the window sent it: without the request id and FakeMaild's connection number.
inline QJsonObject body(QJsonObject r)
{
    r.remove("id");
    r.remove("conn");
    return r;
}

inline QJsonObject inboxList(int offset, int limit)
{
    return {{"cmd", "list"}, {"role", "inbox"}, {"unread_only", false}, {"offset", offset}, {"limit", limit}};
}

// A server search hit in Gmail's All Mail.
inline QJsonObject hit(const QString &subject, int uid)
{
    return {{"id", 0}, {"account", "me@gmail.com"}, {"role", "remote"}, {"folder_path", "[Gmail]/All Mail"},
            {"date", 1700000000}, {"from", QJsonObject{{"name", "Ozon"}, {"addr", "news@ozon.example"}}},
            {"subject", subject}, {"preview", ""}, {"seen", true}, {"flagged", false}, {"has_attachments", false},
            {"queued", false}, {"failed", false},
            {"remote", QJsonObject{{"account", "me@gmail.com"}, {"folder_path", "[Gmail]/All Mail"}, {"uid", uid}}}};
}

struct AppHarness {
    FakeMaild *d = nullptr;
    Client *c = nullptr;
    QTemporaryDir *dir = nullptr;
    Images *images = nullptr;
    Renderer *renderer = nullptr;
    Theme *theme = nullptr;
    Letter *letter = nullptr;
    MailList *list = nullptr;
    Compose *compose = nullptr;
    Nav *nav = nullptr;
    App *app = nullptr;
    QJsonArray accounts, rows;
    QJsonObject folders;
    bool offline = false;

    QList<QJsonObject> sent(const QString &cmd) const
    {
        QList<QJsonObject> out;
        for (const QJsonObject &r : d->requests())
            if (r.value("cmd").toString() == cmd)
                out << r;
        return out;
    }
    QJsonArray slice(const QJsonObject &listReq) const
    {
        QJsonArray out;
        const int limit = listReq.value("limit").toInt();
        for (int i = listReq.value("offset").toInt(); i < rows.size() && out.size() < limit; ++i)
            out << rows.at(i);
        return out;
    }
    void boot(qint64 openId = 0)
    {
        app->start(openId);
        c->start();
    }
    // Requests on the window's connection are answered in order: once this one has arrived, so has every earlier one.
    void barrier()
    {
        const auto n = sent("contacts").size();
        app->suggest("zz");
        QTRY_COMPARE(sent("contacts").size(), n + 1);
    }

    void setUp()
    {
        launch::setProgramForTests("/nonexistent/uwsm-app");   // never the real uwsm-app / browser
        accounts = fixture("accounts.json").array();
        rows = fixture("list.json").array();
        folders = fixture("folders.json").object();
        QCOMPARE(rows.size(), 60);
        offline = false;
        d = new FakeMaild;
        QVERIFY(d->start());
        d->on("accounts", [this](const QJsonObject &) { return QJsonObject{{"result", accounts}}; });
        d->on("status", [this](const QJsonObject &) {
            QJsonArray st;
            for (const QJsonValue &a : std::as_const(accounts))
                st << QJsonObject{{"email", a.toObject().value("email")},
                                  {"status", offline ? QJsonValue(QJsonObject{{"offline", "no route"}}) : QJsonValue("online")},
                                  {"queued", offline ? 3 : 0}};
            return QJsonObject{{"result", QJsonObject{{"accounts", st}}}};
        });
        d->on("folders", [this](const QJsonObject &r) {
            const QString a = r.value("account").toString();
            return QJsonObject{{"result", folders.value(a.isEmpty() ? QStringLiteral("unified") : a)}};
        });
        d->on("list", [this](const QJsonObject &r) { return QJsonObject{{"result", slice(r)}}; });
        d->on("message", [](const QJsonObject &r) {
            return QJsonObject{{"result", QJsonObject{{"summary", QJsonObject{{"id", r.value("message")}}},
                                                      {"text", "hi"}, {"html", QJsonValue::Null}}}};
        });
        c = new Client(d->path());
        c->setRetryInterval(50);
        dir = new QTemporaryDir(scratch());
        images = new Images(dir->filePath("img"));
        renderer = new Renderer;
        theme = new Theme;
        letter = new Letter(c, images, renderer, theme);
        list = new MailList;
        compose = new Compose(dir->filePath("drafts"));
        nav = new Nav;
        app = new App(c, list, letter, compose, nav, theme);
    }
    void tearDown()
    {
        delete app;
        delete nav;
        delete compose;
        delete list;
        delete letter;
        delete theme;
        delete renderer;
        delete images;
        delete dir;
        delete c;
        delete d;
    }
};

} // namespace om::test
