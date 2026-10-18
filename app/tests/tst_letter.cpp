#include "Client.h"
#include "FakeMaild.h"
#include "Images.h"
#include "Launch.h"
#include "LaunchStub.h"
#include "Letter.h"
#include "Renderer.h"
#include "Theme.h"
#include <QClipboard>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

using namespace om;
using namespace om::test;

static QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

static QString fixture(const QString &name) { return QStringLiteral(OM_SOURCE_DIR "/tests/fixtures/") + name; }

// message.json as letter `id`; its cid image points at the fixture logo.
static QJsonObject full(qint64 id)
{
    QJsonObject f = QJsonDocument::fromJson(readFile(fixture("message.json"))).object();
    QJsonObject s = f.value("summary").toObject();
    s.insert("id", id);
    f.insert("summary", s);
    f.insert("inline", QJsonObject{{"logo@x", fixture("logo.png")}});
    return f;
}

static QJsonObject withSummary(QJsonObject f, const QJsonObject &fields)
{
    QJsonObject s = f.value("summary").toObject();
    for (auto it = fields.begin(); it != fields.end(); ++it)
        s.insert(it.key(), it.value());
    f.insert("summary", s);
    return f;
}

// A reply whose quoted history is taller than ~120 px: collapsed by default.
static QString tallQuote()
{
    QString lines;
    for (int i = 1; i <= 20; ++i)
        lines += QStringLiteral("Quoted line %1<br>").arg(i);
    return QStringLiteral("<p>See below.</p><blockquote>%1</blockquote>").arg(lines);
}

static bool has(const std::shared_ptr<DocumentContainer> &doc, const QString &text)
{
    bool found = false;
    doc->findText(text, {}, false, nullptr, &found, nullptr, nullptr);
    return found;
}

static QList<QJsonObject> sent(const FakeMaild &d, const QString &cmd)
{
    QList<QJsonObject> out;
    for (const QJsonObject &r : d.requests())
        if (r.value("cmd").toString() == cmd)
            out << r;
    return out;
}

// Serves the fixture logo for any path; with `hold`, answers wait for release().
class ImageServer : public QTcpServer {
public:
    int hits = 0;   // requests, counted once the head is complete (it may come in several reads)
    bool hold = false;
    ImageServer()
    {
        QVERIFY(listen(QHostAddress::LocalHost));
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *s = nextPendingConnection()) {
                auto head = std::make_shared<QByteArray>();
                connect(s, &QTcpSocket::readyRead, s, [this, s, head] {
                    const bool before = head->contains("\r\n\r\n");
                    *head += s->readAll();
                    if (before || !head->contains("\r\n\r\n"))
                        return;
                    ++hits;
                    if (hold)
                        m_held << s;
                    else
                        answer(s);
                });
                connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            }
        });
    }
    QString url(const QString &path) const { return QStringLiteral("http://127.0.0.1:%1%2").arg(serverPort()).arg(path); }
    void release()
    {
        hold = false;
        for (const QPointer<QTcpSocket> &s : std::exchange(m_held, {}))
            if (s)
                answer(s);
    }

private:
    QList<QPointer<QTcpSocket>> m_held;
    static void answer(QTcpSocket *s)
    {
        const QByteArray body = readFile(fixture("logo.png"));
        s->write("HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nConnection: close\r\nContent-Length: "
                 + QByteArray::number(body.size()) + "\r\n\r\n" + body);
        s->disconnectFromHost();
    }
};

class TstLetter : public QObject {
    Q_OBJECT
    FakeMaild *d = nullptr;
    Client *c = nullptr;
    QTemporaryDir *imgDir = nullptr;
    Images *images = nullptr;
    Renderer *renderer = nullptr;
    Theme *theme = nullptr;
    Letter *letter = nullptr;

    void serveFixture()
    {
        d->on("message", [](const QJsonObject &req) { return QJsonObject{{"result", full(req.value("message").toInteger())}}; });
    }

private Q_SLOTS:
    void init()
    {
        launch::setProgramForTests("/nonexistent/uwsm-app"); // never the real uwsm-app / browser
        d = new FakeMaild;
        QVERIFY(d->start());
        c = new Client(d->path());
        c->start();
        QTRY_VERIFY_WITH_TIMEOUT(c->connected(), 2000);
        imgDir = new QTemporaryDir(scratch());
        images = new Images(imgDir->path());
        renderer = new Renderer;
        theme = new Theme;
        letter = new Letter(c, images, renderer, theme);
        letter->setWidth(760);
    }
    void cleanup()
    {
        delete letter;
        delete renderer;
        delete images;
        delete imgDir;
        delete theme;
        delete c;
        delete d;
    }
    void show_requests_message_on_own_connection()
    {
        serveFixture();
        QSignalSpy docs(letter, &Letter::documentChanged);
        letter->show({{"id", 42}});
        QCOMPARE(letter->state(), QString("loading"));
        QVERIFY(d->waitFor("message"));
        const QJsonObject req = sent(*d, "message").value(0);
        QCOMPARE(req.value("message").toInteger(), 42);
        QVERIFY(req.value("conn").toInt() != sent(*d, "subscribe").value(0).value("conn").toInt());

        QTRY_COMPARE(letter->state(), QString());
        QCOMPARE(letter->id(), 42);
        QCOMPARE(letter->subject(), QString("Счёт за октябрь"));
        QCOMPARE(letter->from(), QString("Анна Смирнова <anna@example.com>"));
        QCOMPARE(letter->to(), QString("me@gmail.com, boss@example.com"));
        QVERIFY(!letter->date().isEmpty());
        QCOMPARE(letter->outbox(), 0);
        const QVariantList atts = letter->attachments();
        QCOMPARE(atts.size(), 2);
        const QVariantMap a = atts.at(0).toMap(), b = atts.at(1).toMap();
        QCOMPARE(a.value("index").toInt(), 0);
        QCOMPARE(a.value("name").toString(), QString("a.pdf"));
        QCOMPARE(a.value("icon").toString(), QString("󰈙"));
        QVERIFY(!a.value("sizeText").toString().isEmpty());
        QCOMPARE(a.value("busy").toBool(), false);
        QCOMPARE(b.value("index").toInt(), 2);
        QCOMPARE(b.value("icon").toString(), QString("󰋩"));

        QTRY_COMPARE_WITH_TIMEOUT(docs.size(), 1, 10000);
        QCOMPARE(docs.at(0).at(0).toBool(), false);
        QVERIFY(letter->document());
        letter->setWidth(500); // relayout of the same letter keeps its scroll
        QTRY_COMPARE_WITH_TIMEOUT(docs.size(), 2, 10000);
        QCOMPARE(docs.at(1).at(0).toBool(), true);
        QVERIFY(letter->document()->documentWidth() <= 500);
    }
    void stale_reply_dropped()
    {
        d->on("message", [](const QJsonObject &req) {
            const qint64 id = req.value("message").toInteger();
            QJsonObject r{{"result", withSummary(full(id), {{"subject", id == 1 ? "one" : "two"}})}};
            if (id == 1)
                r.insert("delay", 200);
            return r;
        });
        letter->show({{"id", 1}});
        letter->show({{"id", 2}});
        QTRY_COMPARE(letter->subject(), QString("two"));
        // the late reply to letter 1 has arrived once its connection is gone
        QTRY_VERIFY(c->findChildren<QLocalSocket *>(Qt::FindDirectChildrenOnly).isEmpty());
        QCOMPARE(letter->id(), 2);
        QCOMPARE(letter->subject(), QString("two"));
    }
    void remote_open_then_message()
    {
        d->on("open_remote", [](const QJsonObject &) { return QJsonObject{{"result", QJsonObject{{"id", 230}}}}; });
        serveFixture();
        const QJsonObject hit{{"id", 0}, {"role", "remote"}, {"subject", "old"},
                              {"remote", QJsonObject{{"account", "a@x"}, {"folder_path", "[Gmail]/All Mail"}, {"uid", 7}}}};
        QElapsedTimer timer;
        timer.start();
        letter->show(hit);
        QCOMPARE(letter->state(), QString("remote"));
        QCOMPARE(letter->subject(), QString("old"));
        QVERIFY(d->waitFor("open_remote"));
        QVERIFY2(timer.elapsed() >= 250, qPrintable(QString::number(timer.elapsed())));
        const QJsonObject open = sent(*d, "open_remote").value(0);
        QCOMPARE(open.value("account").toString(), QString("a@x"));
        QCOMPARE(open.value("folder_path").toString(), QString("[Gmail]/All Mail"));
        QCOMPARE(open.value("uid").toInt(), 7);
        QVERIFY(d->waitFor("message"));
        QCOMPARE(sent(*d, "message").value(0).value("message").toInteger(), 230);
        QTRY_COMPARE(letter->state(), QString());
        QCOMPARE(letter->id(), 230);
    }
    void remote_open_waits_for_quiet()
    {
        d->on("open_remote", [](const QJsonObject &) { return QJsonObject{{"result", QJsonObject{{"id", 230}}}}; });
        serveFixture();
        const auto hit = [](int uid) {
            return QJsonObject{{"id", 0}, {"remote", QJsonObject{{"account", "a@x"}, {"folder_path", "INBOX"}, {"uid", uid}}}};
        };
        letter->show(hit(7));
        QTest::qWait(100);
        letter->show(hit(8));
        QVERIFY(d->waitFor("message"));
        const auto opens = sent(*d, "open_remote");
        QCOMPARE(opens.size(), 1);
        QCOMPARE(opens.at(0).value("uid").toInt(), 8);
    }
    void outbox_failed_state()
    {
        d->on("message", [](const QJsonObject &req) {
            const qint64 id = req.value("message").toInteger();
            QJsonObject f = withSummary(full(id), {{"queued", true}, {"failed", id == -3}});
            if (id == -3)
                f.insert("send_error", "550 rejected");
            return QJsonObject{{"result", f}};
        });
        letter->show({{"id", -4}, {"queued", true}});
        QCOMPARE(letter->outbox(), 1);
        letter->show({{"id", -3}, {"queued", true}, {"failed", true}});
        QCOMPARE(letter->outbox(), 2);
        QTRY_COMPARE(letter->sendError(), QString("550 rejected"));
        QCOMPARE(letter->outbox(), 2);

        QSignalSpy notes(letter, &Letter::note);
        letter->openAttachment(0);
        QCOMPARE(notes.size(), 1);
        QCOMPARE(notes.at(0).at(0).toString(), QString("Attachments of unsent mail can't be opened"));
        QCOMPARE(notes.at(0).at(1).toBool(), true);
        QVERIFY(sent(*d, "attachment").isEmpty());
    }
    void links_skip_anchors_with_no_box()
    {
        // an empty anchor, or one around an image that isn't there, has nowhere to scroll to
        d->on("message", [](const QJsonObject &req) {
            QJsonObject f = full(req.value("message").toInteger());
            f.insert("html", "<p><a href=\"https://a.example/\"></a> one <a href=\"https://c.example/\"><img src=\"cid:missing\"></a>"
                             " two <a href=\"https://b.example/\">b</a></p>");
            return QJsonObject{{"result", f}};
        });
        QSignalSpy docs(letter, &Letter::documentChanged);
        letter->show({{"id", 42}});
        QTRY_COMPARE_WITH_TIMEOUT(docs.size(), 1, 10000);
        QCOMPARE(letter->linkCount(), 1);
        QCOMPARE(letter->links().at(0).url, QUrl("https://b.example/"));
    }
    void plain_text_links_and_mail()
    {
        d->on("message", [](const QJsonObject &req) {
            QJsonObject f = full(req.value("message").toInteger());
            f.insert("html", "");
            f.insert("text", "Write ivan@x.example or see www.site.example.");
            return QJsonObject{{"result", f}};
        });
        QSignalSpy docs(letter, &Letter::documentChanged);
        letter->show({{"id", 42}});
        QTRY_COMPARE_WITH_TIMEOUT(docs.size(), 1, 10000);
        QCOMPARE(letter->linkCount(), 2);
        QCOMPARE(letter->links().at(0).url, QUrl("mailto:ivan@x.example"));
        QCOMPARE(letter->links().at(1).url, QUrl("https://www.site.example"));
        QSignalSpy compose(letter, &Letter::composeTo);
        letter->activateLink(letter->links().at(0).url);
        QCOMPARE(compose.size(), 1);
    }
    void links_only_http_https_mailto()
    {
        QTemporaryDir dir(scratch());
        const QString stub = writeStub(dir.path());
        launch::setProgramForTests(stub);
        QSignalSpy compose(letter, &Letter::composeTo);
        for (const char *url : {"javascript:alert(1)", "file:///etc/passwd", "data:text/html,x"})
            letter->activateLink(QUrl(url));
        letter->activateLink(QUrl("https://x.org"));
        // a forbidden link launched first would show up as an extra line
        QTRY_COMPARE(stubCalls(stub), (QList<QStringList>{{"--", "xdg-open", "https://x.org"}}));
        letter->activateLink(QUrl("mailto:a@x.org?subject=Hi"));
        QCOMPARE(compose.size(), 1);
        QCOMPARE(compose.at(0).at(0).toUrl(), QUrl("mailto:a@x.org?subject=Hi"));
        QCOMPARE(stubCalls(stub).size(), 1);
    }
    void open_attachment_fetches_then_launches()
    {
        QTemporaryDir dir(scratch());
        const QString stub = writeStub(dir.path());
        launch::setProgramForTests(stub);
        serveFixture();
        d->on("attachment", [](const QJsonObject &req) {
            const int index = req.value("index").toInt();
            const QString path = QStringLiteral("/c/%1/%2/%3")
                                     .arg(req.value("message").toInteger())
                                     .arg(index)
                                     .arg(index == 0 ? "a.pdf" : "photo.jpg");
            return QJsonObject{{"result", QJsonObject{{"path", path}}}};
        });
        letter->show({{"id", 42}});
        QTRY_COMPARE(letter->attachments().size(), 2);
        letter->openAttachment(0);
        QCOMPARE(letter->attachments().at(0).toMap().value("busy").toBool(), true);
        QTRY_COMPARE(stubCalls(stub), (QList<QStringList>{{"--", "xdg-open", "/c/42/0/a.pdf"}}));
        const QJsonObject req = sent(*d, "attachment").value(0);
        QCOMPARE(req.value("message").toInteger(), 42);
        QCOMPARE(req.value("index").toInt(), 0);
        QCOMPARE(letter->attachments().at(0).toMap().value("busy").toBool(), false);

        letter->openAttachment(1); // the second shown one is attachment 2: 1 is inline
        QTRY_COMPARE(stubCalls(stub).size(), 2);
        QCOMPARE(stubCalls(stub).at(1), (QStringList{"--", "xdg-open", "/c/42/2/photo.jpg"}));
    }
    void copy_text_sets_clipboard()
    {
        d->on("message", [](const QJsonObject &req) {
            const qint64 id = req.value("message").toInteger();
            QJsonObject f = full(id);
            if (id == 43) {
                f.insert("text", "");
                f.insert("html", "<p>Hello <b>world</b></p>");
            }
            return QJsonObject{{"result", f}};
        });
        letter->show({{"id", 42}});
        QTRY_VERIFY(!letter->full().isEmpty());
        letter->copyText();
        QCOMPARE(QGuiApplication::clipboard()->text(), full(42).value("text").toString());

        letter->show({{"id", 43}});
        QTRY_VERIFY(!letter->full().isEmpty());
        letter->copyText();
        QTRY_COMPARE(QGuiApplication::clipboard()->text(), QString("Hello world"));
    }
    void remote_images_second_pass()
    {
        ImageServer srv;
        const QString url = srv.url("/a.png");
        d->on("message", [url](const QJsonObject &req) {
            QJsonObject f = full(req.value("message").toInteger());
            f.insert("html", QStringLiteral("<p>hi</p><img src=\"%1\" width=\"10\" height=\"10\">").arg(url));
            return QJsonObject{{"result", f}};
        });
        QSignalSpy docs(letter, &Letter::documentChanged);
        letter->show({{"id", 42}});
        QTRY_COMPARE_WITH_TIMEOUT(docs.size(), 2, 10000);
        QCOMPARE(docs.at(0).at(0).toBool(), false);
        QCOMPARE(docs.at(1).at(0).toBool(), true);
        QCOMPARE(srv.hits, 1);
        QVERIFY(QFile::exists(Images::pathFor(imgDir->path(), QUrl(url))));
    }
    void quoted_history_expands_for_that_letter()
    {
        d->on("message", [](const QJsonObject &req) {
            QJsonObject f = full(req.value("message").toInteger());
            f.insert("html", tallQuote());
            return QJsonObject{{"result", f}};
        });
        QSignalSpy docs(letter, &Letter::documentChanged);
        letter->show({{"id", 42}});
        QTRY_COMPARE_WITH_TIMEOUT(docs.size(), 1, 10000);
        QVERIFY(!has(letter->document(), "Quoted line 7"));
        QCOMPARE(letter->linkCount(), 1);
        QCOMPARE(letter->links().at(0).url, QUrl("omarchy-mail:quotes/0"));

        letter->activateLink(letter->links().at(0).url);
        QTRY_COMPARE_WITH_TIMEOUT(docs.size(), 2, 10000);
        QCOMPARE(docs.at(1).at(0).toBool(), true);   // the same letter: the view keeps its scroll
        QVERIFY(has(letter->document(), "Quoted line 7"));
        QCOMPARE(letter->linkCount(), 0);
        letter->reload();   // auto-read reloads it: still open
        QTRY_COMPARE_WITH_TIMEOUT(docs.size(), 3, 10000);
        QVERIFY(has(letter->document(), "Quoted line 7"));

        letter->show({{"id", 43}});   // another letter: folded again
        QTRY_COMPARE_WITH_TIMEOUT(docs.size(), 5, 10000);   // cleared, then laid out
        QVERIFY(!has(letter->document(), "Quoted line 7"));
        letter->show({{"id", 42}});
        QTRY_COMPARE_WITH_TIMEOUT(docs.size(), 7, 10000);
        QVERIFY(!has(letter->document(), "Quoted line 7"));
    }
    void reload_keeps_image_batch()
    {
        // an unread letter is reloaded ~1 s after opening, often while its images are still on the way
        ImageServer srv;
        srv.hold = true;
        const QString url = srv.url("/a.png");
        d->on("message", [url](const QJsonObject &req) {
            QJsonObject f = full(req.value("message").toInteger());
            f.insert("html", QStringLiteral("<p>hi</p><img src=\"%1\" width=\"10\" height=\"10\">").arg(url));
            return QJsonObject{{"result", f}};
        });
        QSignalSpy docs(letter, &Letter::documentChanged);
        letter->show({{"id", 42}});
        QTRY_COMPARE_WITH_TIMEOUT(docs.size(), 1, 10000);
        QTRY_COMPARE(srv.hits, 1);
        letter->reload();
        QTRY_COMPARE_WITH_TIMEOUT(docs.size(), 2, 10000);
        srv.release();
        QTRY_COMPARE_WITH_TIMEOUT(docs.size(), 3, 10000); // the first batch's images are laid out
        QCOMPARE(docs.at(1).at(0).toBool(), true);
        QCOMPARE(docs.at(2).at(0).toBool(), true);
        QCOMPARE(srv.hits, 1); // not requested again by the reload
        QVERIFY(QFile::exists(Images::pathFor(imgDir->path(), QUrl(url))));
    }
};

QTEST_MAIN(TstLetter)
#include "tst_letter.moc"
