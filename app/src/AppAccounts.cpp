// App's mailboxes page: add, remove, retry the login, rename the sender.
#include "App.h"
#include "Client.h"
#include "I18n.h"
#include <QDateTime>
#include <QPointer>

namespace om {

namespace {
// {host, port} from the form, empty when either is missing or the port is out of range.
QJsonObject server(const QVariantMap &form, const QString &host, const QString &port)
{
    bool ok = false;
    const int p = form.value(port).toString().trimmed().toInt(&ok);
    const QString h = form.value(host).toString().trimmed();
    if (h.isEmpty() || !ok || p < 1 || p > 65535)
        return {};
    return {{"host", h}, {"port", p}};
}
} // namespace

// The request carries the password: it is never logged, and the page clears its field after every attempt.
void App::addAccount(const QVariantMap &form)
{
    if (m_adding)
        return;   // one trial login at a time, however fast the second Enter comes
    const QString provider = form.value(QStringLiteral("provider")).toString();
    const QString email = form.value(QStringLiteral("email")).toString().trimmed();
    const QString password = form.value(QStringLiteral("password")).toString();
    QJsonObject req{{"cmd", "account_add"}, {"provider", provider}, {"email", email},
                    {"name", form.value(QStringLiteral("name")).toString().trimmed()}, {"password", password}};
    QString err;
    if (!email.contains(QLatin1Char('@'))) {
        err = t("Enter the email address");
    } else if (password.isEmpty()) {
        err = t("Enter the app password");
    } else if (provider == QLatin1String("imap")) {   // gmail and yandex: maild has their servers
        const QJsonObject imap = server(form, QStringLiteral("imapHost"), QStringLiteral("imapPort"));
        const QJsonObject smtp = server(form, QStringLiteral("smtpHost"), QStringLiteral("smtpPort"));
        if (imap.isEmpty() || smtp.isEmpty())
            err = t("Fill in the server and port");
        req.insert(QStringLiteral("imap"), imap);
        req.insert(QStringLiteral("smtp"), smtp);
        const QString login = form.value(QStringLiteral("login")).toString().trimmed();
        if (!login.isEmpty())   // empty: maild logs in with the email
            req.insert(QStringLiteral("login"), login);
    }
    m_addError = err;
    m_adding = err.isEmpty();
    Q_EMIT changed();
    if (!m_adding)
        return;
    m_client->requestSlow(req, [self = QPointer<App>(this)](const QJsonValue &, const QString &e) {   // up to ~70 s
        if (!self)
            return;
        self->m_adding = false;
        self->m_addError = e;
        if (e.isEmpty()) {
            Q_EMIT self->accountAdded();
            self->connectedChanged();   // the mailboxes, their folders and the list anew
        }
        Q_EMIT self->changed();
    });
}

void App::removeAccount(const QString &email)
{
    if (!m_confirm.press(QStringLiteral("remove ") + email, QDateTime::currentMSecsSinceEpoch())) {
        setNote(t("Press d again to remove %1 (mail on the server stays)").arg(email), false);
        return;
    }
    m_client->request({{"cmd", "account_remove"}, {"email", email}}, ok([this](const QJsonValue &) { connectedChanged(); }));
}

void App::retryAccount(const QString &email)
{
    m_client->request({{"cmd", "sync"}, {"account", email}}, ok([](const QJsonValue &) {}));
}

void App::renameAccount(const QString &email, const QString &name)
{
    m_client->request({{"cmd", "account_update"}, {"email", email}, {"name", name}}, ok([this](const QJsonValue &) {
        m_client->request({{"cmd", "accounts"}}, ok([this](const QJsonValue &r) {
            m_accounts = r.toArray();
            Q_EMIT changed();
        }));
    }));
}

} // namespace om
