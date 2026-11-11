#include "AppHarness.h"
#include <QSignalSpy>

using namespace om;
using namespace om::test;

// App part 3: the mailboxes page — add and save (on their own connection), remove, retry, rename.
class TstAppAccounts : public QObject, AppHarness {
    Q_OBJECT

    bool saving(int i) const { return app->accounts().at(i).toMap().value("saving").toBool(); }
    QString saveError(int i) const { return app->accounts().at(i).toMap().value("saveError").toString(); }

    static QVariantMap gmail()
    {
        return {{"provider", "gmail"}, {"email", " new@gmail.com "}, {"name", "Max"}, {"password", "abcd efgh"}};
    }
    static QVariantMap imap()
    {
        return {{"provider", "imap"}, {"email", "me@example.org"}, {"name", "Max"}, {"password", "pw"}, {"login", ""},
                {"imapHost", "mail.example.org"}, {"imapPort", "993"}, {"smtpHost", "mail.example.org"},
                {"smtpPort", "465"}};
    }
    void bootList()
    {
        boot();
        QTRY_COMPARE(list->rowCount(), 50);
    }

private Q_SLOTS:
    void init() { setUp(); }
    void cleanup() { tearDown(); }

    void add_gmail_request_shape()
    {
        d->on("account_add", [](const QJsonObject &r) { return QJsonObject{{"result", QJsonObject{{"email", r.value("email")}}}}; });
        bootList();
        QSignalSpy added(app, &App::accountAdded);
        const auto accountsBefore = sent("accounts").size();
        app->addAccount(gmail());
        QVERIFY(app->adding());
        QTRY_COMPARE(sent("account_add").size(), 1);
        const QJsonObject r = sent("account_add").at(0);
        QCOMPARE(body(r), QJsonObject({{"cmd", "account_add"}, {"provider", "gmail"}, {"email", "new@gmail.com"},
                                       {"name", "Max"}, {"password", "abcd efgh"}}));
        QVERIFY(r.value("conn") != sent("subscribe").at(0).value("conn"));   // ~70 s: never blocks the list
        QTRY_COMPARE(added.size(), 1);
        QVERIFY(!app->adding());
        QCOMPARE(app->addError(), QString());
        QTRY_VERIFY(sent("accounts").size() > accountsBefore);   // the mailboxes, their folders and the list anew
    }

    void add_imap_request_shape()
    {
        d->on("account_add", [](const QJsonObject &) { return QJsonObject{{"result", QJsonObject{}}}; });
        bootList();
        app->addAccount(imap());
        QTRY_COMPARE(sent("account_add").size(), 1);
        QCOMPARE(body(sent("account_add").at(0)),
                 QJsonObject({{"cmd", "account_add"}, {"provider", "imap"}, {"email", "me@example.org"}, {"name", "Max"},
                              {"password", "pw"}, {"imap", QJsonObject{{"host", "mail.example.org"}, {"port", 993}}},
                              {"smtp", QJsonObject{{"host", "mail.example.org"}, {"port", 465}}}}));
        QVariantMap withLogin = imap();
        withLogin["login"] = "me";
        app->addAccount(withLogin);
        QTRY_COMPARE(sent("account_add").size(), 2);
        QCOMPARE(sent("account_add").at(1).value("login").toString(), QString("me"));
    }

    void add_validation()
    {
        bootList();
        const auto refused = [this](const QVariantMap &form) {
            app->addAccount(form);
            QVERIFY(!app->adding());
            QVERIFY(!app->addError().isEmpty());
        };
        QVariantMap f = gmail();
        f["email"] = "new.gmail.com";
        refused(f);
        f = gmail();
        f["password"] = "";
        refused(f);
        for (const char *k : {"imapHost", "smtpHost"}) {
            f = imap();
            f[k] = " ";
            refused(f);
            QCOMPARE(app->addError(), QString("Fill in the server and port"));
        }
        for (const char *port : {"0", "65536", "x", ""}) {
            f = imap();
            f["smtpPort"] = port;
            refused(f);
        }
        barrier();
        QVERIFY(sent("account_add").isEmpty());
    }

    void add_error_from_server()
    {
        d->on("account_add", [](const QJsonObject &) { return QJsonObject{{"error", "AUTHENTICATIONFAILED"}, {"delay", 50}}; });
        bootList();
        QSignalSpy added(app, &App::accountAdded);
        app->addAccount(gmail());
        QVERIFY(app->adding());
        app->addAccount(gmail());   // a second Enter while the login is checked: one request
        QTRY_VERIFY(!app->adding());
        QCOMPARE(app->addError(), QString("AUTHENTICATIONFAILED"));
        QCOMPARE(sent("account_add").size(), 1);
        QCOMPARE(added.size(), 0);
        app->addAccount(gmail());   // the next attempt starts clean
        QCOMPARE(app->addError(), QString());
    }

    void remove_double_press()
    {
        d->on("account_remove", [](const QJsonObject &) { return QJsonObject{}; });
        bootList();
        app->removeAccount("me@yandex.ru");
        QCOMPARE(app->note(), QString("Press d again to remove me@yandex.ru (mail on the server stays)"));
        app->removeAccount("me@gmail.com");   // another mailbox: its own first press
        barrier();
        QVERIFY(sent("account_remove").isEmpty());
        const auto accountsBefore = sent("accounts").size();
        app->removeAccount("me@gmail.com");
        QTRY_COMPARE(sent("account_remove").size(), 1);
        QCOMPARE(body(sent("account_remove").at(0)), QJsonObject({{"cmd", "account_remove"}, {"email", "me@gmail.com"}}));
        QTRY_VERIFY(sent("accounts").size() > accountsBefore);
    }

    void retry_auth_failed()
    {
        bootList();
        const auto before = sent("sync").size();
        app->retryAccount("me@yandex.ru");
        QTRY_COMPARE(sent("sync").size(), before + 1);
        QCOMPARE(body(sent("sync").last()), QJsonObject({{"cmd", "sync"}, {"account", "me@yandex.ru"}}));
    }

    void rename_request_shape()
    {
        d->on("account_update", [](const QJsonObject &) { return QJsonObject{}; });
        bootList();
        const auto accountsBefore = sent("accounts").size();
        app->renameAccount("me@gmail.com", "Max Petrov");
        QTRY_COMPARE(sent("account_update").size(), 1);
        QCOMPARE(body(sent("account_update").at(0)),
                 QJsonObject({{"cmd", "account_update"}, {"email", "me@gmail.com"}, {"name", "Max Petrov"}}));
        QTRY_VERIFY(sent("accounts").size() > accountsBefore);   // the page shows the new name
    }

    void update_request_shape()
    {
        d->on("account_update", [](const QJsonObject &) { return QJsonObject{}; });
        bootList();
        const auto accountsBefore = sent("accounts").size();
        QVariantMap form{{"email", "me@gmail.com"}, {"name", " Max P "}, {"password", ""}, {"login", "x"},
                         {"imapHost", "h"}, {"imapPort", "1"}, {"smtpHost", "h"}, {"smtpPort", "1"}};
        app->updateAccount(form);
        QVERIFY(saving(0));
        QTRY_COMPARE(sent("account_update").size(), 1);
        const QJsonObject r = sent("account_update").at(0);
        // gmail sends no servers or login; an empty password is not sent
        QCOMPARE(body(r), QJsonObject({{"cmd", "account_update"}, {"email", "me@gmail.com"}, {"name", "Max P"}}));
        QVERIFY(r.value("conn") != sent("subscribe").at(0).value("conn"));   // a trial login never blocks the list
        QTRY_VERIFY(sent("accounts").size() > accountsBefore);
        QVERIFY(!saving(0));
        form["password"] = "new pw";
        app->updateAccount(form);
        QTRY_COMPARE(sent("account_update").size(), 2);
        QCOMPARE(sent("account_update").at(1).value("password").toString(), QString("new pw"));
    }

    void update_imap_request_shape()
    {
        d->on("account_update", [](const QJsonObject &) { return QJsonObject{}; });
        accounts.append(QJsonObject{{"email", "me@example.org"}, {"name", "Max"}, {"provider", "imap"}, {"login", "me"},
                                    {"imap", QJsonObject{{"host", "mail.example.org"}, {"port", 993}}},
                                    {"smtp", QJsonObject{{"host", "mail.example.org"}, {"port", 465}}},
                                    {"append_sent", true}});
        bootList();
        QVariantMap form{{"email", "me@example.org"}, {"name", "Max"}, {"password", ""}, {"login", " me2 "},
                         {"imapHost", "imap.example.org"}, {"imapPort", "143"}, {"smtpHost", "smtp.example.org"},
                         {"smtpPort", "587"}};
        app->updateAccount(form);
        QTRY_COMPARE(sent("account_update").size(), 1);
        QCOMPARE(body(sent("account_update").at(0)),
                 QJsonObject({{"cmd", "account_update"}, {"email", "me@example.org"}, {"name", "Max"}, {"login", "me2"},
                              {"imap", QJsonObject{{"host", "imap.example.org"}, {"port", 143}}},
                              {"smtp", QJsonObject{{"host", "smtp.example.org"}, {"port", 587}}}}));
        QTRY_VERIFY(!saving(2));
        form["imapPort"] = "x";
        app->updateAccount(form);
        QCOMPARE(saveError(2), QString("Fill in the server and port"));
        QVERIFY(!saving(2));
        app->updateAccount({{"email", "nobody@example.org"}, {"name", "Max"}, {"password", "pw"}});   // unknown: nothing
        barrier();
        QCOMPARE(sent("account_update").size(), 1);
    }

    void update_error_stays_with_its_mailbox()
    {
        d->on("account_update", [](const QJsonObject &) { return QJsonObject{{"error", "AUTHENTICATIONFAILED"}, {"delay", 50}}; });
        bootList();
        const QVariantMap bad{{"email", "me@gmail.com"}, {"name", "Max"}, {"password", "bad"}};
        app->updateAccount(bad);
        app->updateAccount({{"email", "me@yandex.ru"}, {"name", "Max"}, {"password", "pw"}});   // one trial login at a time
        QVERIFY(saving(0));
        QVERIFY(!saving(1));
        QTRY_VERIFY(!saving(0));
        QCOMPARE(sent("account_update").size(), 1);
        QCOMPARE(saveError(0), QString("AUTHENTICATIONFAILED"));
        QCOMPARE(saveError(1), QString());
        app->updateAccount(bad);   // the next save starts clean
        QCOMPARE(saveError(0), QString());
    }
};

QTEST_MAIN(TstAppAccounts)
#include "tst_app_accounts.moc"
