#pragma once
// Compose form model: local drafts (one per new mail / per reply) and pasted screenshots.
#include "Nav.h"
#include <QJsonObject>
#include <QList>
#include <QLocale>
#include <QObject>
#include <QPair>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

namespace om {

struct MailtoDraft {
    QString to, subject, body;
};
MailtoDraft parseMailto(const QString &mailto);

class Compose : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool open READ isOpen NOTIFY changed)
    Q_PROPERTY(bool isReply READ isReply NOTIFY changed)
    Q_PROPERTY(QString account READ account WRITE setAccount NOTIFY changed)
    Q_PROPERTY(QString to READ to WRITE setTo NOTIFY changed)
    Q_PROPERTY(QString subject READ subject WRITE setSubject NOTIFY changed)
    Q_PROPERTY(QString body READ body WRITE setBody NOTIFY changed)
    Q_PROPERTY(int bodyCursor READ bodyCursor NOTIFY changed)
    Q_PROPERTY(QVariantList attachments READ attachments NOTIFY changed)
    Q_PROPERTY(QVariantList suggestions READ suggestions WRITE setSuggestions NOTIFY changed)
    Q_PROPERTY(QString note READ note NOTIFY changed)
    Q_PROPERTY(bool noteError READ noteError NOTIFY changed)
public:
    explicit Compose(QString draftsDir = defaultDir(), QObject *parent = nullptr);
    static QString defaultDir();

    bool isOpen() const { return m_open; }
    bool isReply() const { return m_replyId != 0; }
    QString account() const { return m_account; }
    QString to() const { return m_to; }
    QString subject() const { return m_subject; }
    QString body() const { return m_body; }
    int bodyCursor() const { return m_cursor; }
    QVariantList attachments() const;
    QVariantList suggestions() const { return m_suggestions; }
    QString note() const { return m_note; }
    bool noteError() const { return m_noteError; }
    void setAccount(const QString &v) { edit(m_account, v); }
    void setTo(const QString &v) { edit(m_to, v); }
    void setSubject(const QString &v) { edit(m_subject, v); }
    void setBody(const QString &v) { edit(m_body, v); }
    void setSuggestions(const QVariantList &s);

    void startNew(const QString &account);
    void startReply(const QJsonObject &full);
    void startMailto(const QUrl &url, const QString &account);
    Q_INVOKABLE void close();
    Q_INVOKABLE void discard();
    Q_INVOKABLE void addFiles(const QList<QUrl> &urls);
    Q_INVOKABLE bool paste();
    Q_INVOKABLE void removeAttachment(int i);
    Q_INVOKABLE void pickSuggestion(int i);
    QJsonObject request(QString *error) const;
    void sent();

    static QList<QPair<QString, QString>> parseAddresses(const QString &field, QString *bad);
    static QString replySubject(const QString &subject);
    static QString quote(const QJsonObject &full, const QLocale &time);
    static QString completeTo(const QString &field, const QString &name, const QString &addr);

Q_SIGNALS:
    void changed();

private:
    struct Att {
        QString path;
        bool pasted = false;
    };
    void edit(QString &field, const QString &v);
    void begin(const QString &key, const QString &account);
    void load();
    void save();
    void removeDraft();
    void setNote(const QString &text, bool error = false);
    QString draftPath() const { return m_dir + QLatin1Char('/') + m_key + QStringLiteral(".json"); }

    QString m_dir;
    QString m_key;
    qint64 m_replyId = 0;
    bool m_open = false;
    QString m_account, m_to, m_subject, m_body, m_note;
    bool m_noteError = false;
    int m_cursor = 0;
    QList<Att> m_atts;
    QVariantList m_suggestions;
    QTimer m_saveTimer;
    Confirm m_confirm;
};

} // namespace om
