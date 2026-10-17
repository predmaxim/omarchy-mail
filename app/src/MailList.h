#pragma once
#include <QAbstractListModel>
#include <QColor>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QStringList>
#include <functional>
#include <optional>

namespace om {

class MailList : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int cursor READ cursor WRITE setCursor NOTIFY cursorChanged)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles {
        KeyRole = Qt::UserRole + 1, IdRole, SenderRole, SubjectRole, PreviewRole, SubjectRichRole,
        PreviewRichRole, DateTextRole, PlaceRole, SeenRole, FlaggedRole, AttachmentsRole, QueuedRole,
        FailedRole, RemoteRole, SelectedRole, IsCursorRole, GroupRole
    };

    explicit MailList(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int cursor() const { return cursor_; }
    int selectedCount() const { return selected_.size(); }

    void setRows(const QJsonArray &summaries);
    void appendRows(const QJsonArray &summaries);
    void appendRemote(const QJsonArray &hits);
    void setWords(const QStringList &words, const QColor &mark);
    void setPlace(std::function<QString(const QJsonObject &summary)> place);

    QJsonObject row(int i) const;
    QJsonObject cursorRow() const { return row(cursor_); }
    int initialCursor() const;

    Q_INVOKABLE void move(int dir, bool ctrl, bool shift);
    Q_INVOKABLE void click(int index, bool ctrl, bool shift);
    Q_INVOKABLE void toggleMark();
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE void setCursor(int i);

    QList<qint64> targetIds() const;
    void removeIds(const QList<qint64> &ids);
    void setFlags(const QList<qint64> &ids, std::optional<bool> seen, std::optional<bool> flagged);

Q_SIGNALS:
    void cursorChanged();
    void selectionChanged();
    void countChanged();
    void rowsReplaced(bool sameLetter);   // setRows' reset is over; sameLetter: the cursor's letter is still there

private:
    struct Item {
        QJsonObject json;
        QString key, subjectRich, previewRich, place;
        bool remote = false;
    };
    static QString keyOf(const QJsonObject &j);
    Item makeItem(const QJsonObject &j, bool remote) const;
    void addItems(const QJsonArray &a, bool remote);
    void setSelection(const QSet<QString> &s);
    void setCursorOnly(int i);
    void selectRange(int to);
    void touch(const QList<int> &roles);

    QList<Item> items_;
    QSet<QString> selected_, base_;
    int cursor_ = -1;
    int anchor_ = -1;
    QStringList words_;
    QColor mark_;
    std::function<QString(const QJsonObject &)> place_;
};

} // namespace om
