#include "MailList.h"

#include <QSignalSpy>
#include <QTest>

using namespace om;

static QJsonObject mk(qint64 id, bool seen = true, const QString &subj = {})
{
    return QJsonObject{{"id", id}, {"account", "a"}, {"seen", seen}, {"flagged", false}, {"date", 1700000000},
                       {"subject", subj.isEmpty() ? QString("s%1").arg(id) : subj}, {"preview", "p"},
                       {"from", QJsonObject{{"name", "N"}, {"addr", "n@x.org"}}}};
}
static QJsonArray rows(std::initializer_list<qint64> ids)
{
    QJsonArray a;
    for (qint64 i : ids)
        a.append(mk(i));
    return a;
}

class TstMailList : public QObject
{
    Q_OBJECT
    static QSet<int> sel(MailList &m)
    {
        QSet<int> s;
        for (int i = 0; i < m.rowCount(); ++i)
            if (m.data(m.index(i), MailList::SelectedRole).toBool())
                s.insert(i);
        return s;
    }
    static QVariant role(MailList &m, int i, int r) { return m.data(m.index(i), r); }

private Q_SLOTS:
    void roles_from_summary()
    {
        MailList m;
        QJsonObject a = mk(1);
        a["from"] = QJsonObject{{"name", ""}, {"addr", "z@x.org"}};
        a["queued"] = true;
        a["failed"] = true;
        QJsonObject hit = mk(0);
        hit["remote"] = QJsonObject{{"account", "a"}, {"folder_path", "INBOX"}, {"uid", 5}};
        m.setRows({a, hit});
        QCOMPARE(role(m, 0, MailList::SenderRole).toString(), "z@x.org");
        QCOMPARE(role(m, 0, MailList::KeyRole).toString(), "m1");
        QVERIFY(role(m, 0, MailList::QueuedRole).toBool());
        QVERIFY(role(m, 0, MailList::FailedRole).toBool());
        QVERIFY(!role(m, 0, MailList::RemoteRole).toBool());
        QVERIFY(role(m, 1, MailList::RemoteRole).toBool());
        QCOMPARE(role(m, 1, MailList::GroupRole).toString(), "remote");
        QCOMPARE(role(m, 1, MailList::KeyRole).toString(), "ra/INBOX/5");
        QCOMPARE(role(m, 0, MailList::GroupRole).toString(), "local");
    }
    void reload_signals_only_a_real_cursor_move()   // the view leaves the wheel's place alone otherwise
    {
        MailList m;
        m.setRows(rows({5, 4, 3, 2}));
        m.setCursor(1);
        QSignalSpy moved(&m, &MailList::cursorChanged);
        m.setRows(rows({5, 4, 3, 2}));   // the same rows again
        QCOMPARE(moved.size(), 0);
        m.setRows(rows({6, 5, 4, 3, 2}));   // a new one above: the same letter, one row down
        QCOMPARE(moved.size(), 1);
        QCOMPARE(m.cursor(), 2);
        m.setRows(rows({6, 5, 3, 2}));   // the cursor's letter gone: the same row number, another letter
        QCOMPARE(moved.size(), 2);
        QCOMPARE(m.cursorRow().value("id").toInteger(), 3);
    }
    void plain_move_clears_selection()
    {
        MailList m;
        m.setRows(rows({1, 2, 3}));
        m.setCursor(0);
        m.toggleMark();
        QCOMPARE(m.selectedCount(), 1);
        m.move(1, false, false);
        QCOMPARE(m.cursor(), 1);
        QCOMPARE(m.selectedCount(), 0);
    }
    void ctrl_move_keeps_selection()
    {
        MailList m;
        m.setRows(rows({1, 2, 3}));
        m.setCursor(0);
        m.toggleMark();
        m.move(1, true, false);
        QCOMPARE(m.cursor(), 1);
        QCOMPARE(sel(m), (QSet<int>{0}));
    }
    void ctrl_space_toggles()
    {
        MailList m;
        m.setRows(rows({1, 2}));
        m.setCursor(1);
        m.toggleMark();
        QCOMPARE(sel(m), (QSet<int>{1}));
        m.toggleMark();
        QCOMPARE(m.selectedCount(), 0);
    }
    void shift_range_from_anchor()
    {
        MailList m;
        m.setRows(rows({1, 2, 3, 4, 5}));
        m.setCursor(0);
        m.toggleMark();
        m.move(1, true, false);
        m.move(1, true, false);
        m.toggleMark();
        QCOMPARE(sel(m), (QSet<int>{0, 2}));
        m.move(1, false, true);
        QCOMPARE(sel(m), (QSet<int>{0, 2, 3}));
        m.move(-1, false, true);
        m.move(-1, false, true);
        QCOMPARE(m.cursor(), 1);
        QCOMPARE(sel(m), (QSet<int>{0, 1, 2}));
    }
    void ctrl_a_selects_all()
    {
        MailList m;
        m.setRows(rows({1, 2, 3}));
        m.selectAll();
        QCOMPARE(m.selectedCount(), 3);
    }
    void click_variants()
    {
        MailList m;
        m.setRows(rows({1, 2, 3, 4}));
        m.click(1, false, false);
        QCOMPARE(m.cursor(), 1);
        QCOMPARE(m.selectedCount(), 0);
        m.click(3, true, false);
        QCOMPARE(sel(m), (QSet<int>{3}));
        m.click(0, true, false);
        QCOMPARE(sel(m), (QSet<int>{0, 3}));
        m.click(2, false, true);
        QCOMPARE(sel(m), (QSet<int>{0, 1, 2, 3}));
        m.click(1, false, false);
        QCOMPARE(m.selectedCount(), 0);
        m.click(3, false, true);
        QCOMPARE(sel(m), (QSet<int>{1, 2, 3}));
    }
    void targets_selection_or_cursor()
    {
        MailList m;
        m.setRows(rows({4, 5, -3}));
        QJsonObject hit = mk(0);
        hit["remote"] = QJsonObject{{"account", "a"}, {"folder_path", "I"}, {"uid", 1}};
        m.appendRemote({hit});
        m.setCursor(1);
        QCOMPARE(m.targetIds(), (QList<qint64>{5}));
        m.selectAll();
        QCOMPARE(m.targetIds(), (QList<qint64>{4, 5}));
    }
    void reload_keeps_cursor_by_id()
    {
        MailList m;
        m.setRows(rows({5, 6, 7, 8, 9}));
        m.setCursor(2);
        m.setCursor(2);
        m.click(4, true, false);
        m.click(2, false, false);
        m.click(4, true, false);
        QCOMPARE(m.selectedCount(), 1);
        m.setCursor(2);
        m.setRows(rows({5, 6, 8, 10, 7}));
        QCOMPARE(m.cursor(), 4);
        QCOMPARE(m.selectedCount(), 0);
    }
    void remove_moves_cursor_to_next()
    {
        MailList m;
        m.setRows(rows({1, 2, 3, 4, 5}));
        m.setCursor(2);
        m.removeIds({3});
        QCOMPARE(m.cursor(), 2);
        QCOMPARE(m.cursorRow().value("id").toInteger(), 4);
        m.setCursor(3);
        m.removeIds({5});
        QCOMPARE(m.cursor(), 2);
        QCOMPARE(m.rowCount(), 3);
    }
    void remove_rows_incremental()
    {
        MailList m;
        m.setRows(rows({1, 2, 3, 4, 5}));
        m.setCursor(1);
        m.setCursor(1);
        m.click(3, true, false);
        QSignalSpy rem(&m, &QAbstractItemModel::rowsRemoved);
        QSignalSpy reset(&m, &QAbstractItemModel::modelReset);
        m.removeIds({2, 3, 5});
        QCOMPARE(rem.count(), 2);
        QCOMPARE(reset.count(), 0);
        QCOMPARE(m.rowCount(), 2);
        QCOMPARE(m.cursor(), 1);
        QCOMPARE(m.selectedCount(), 0);
    }
    void remove_nothing_keeps_selection()
    {
        MailList m;
        m.setRows(rows({1, 2, 3}));
        m.selectAll();
        QSignalSpy sig(&m, &MailList::selectionChanged);
        m.removeIds({99});
        QCOMPARE(m.selectedCount(), 3);
        QCOMPARE(sig.count(), 0);
    }
    void initial_cursor_first_unread()
    {
        MailList m;
        QCOMPARE(m.initialCursor(), -1);
        m.setRows({mk(1), mk(2, false), mk(3, false)});
        QCOMPARE(m.initialCursor(), 1);
        m.setRows(rows({1, 2}));
        QCOMPARE(m.initialCursor(), 0);
    }
    void remote_hits_dedup()
    {
        MailList m;
        QJsonObject hit = mk(0);
        hit["remote"] = QJsonObject{{"account", "a"}, {"folder_path", "I"}, {"uid", 1}};
        m.appendRemote({hit});
        m.appendRemote({hit});
        QCOMPARE(m.rowCount(), 1);
    }
    void optimistic_flags()
    {
        MailList m;
        m.setRows({mk(1, false)});
        m.setFlags({1}, true, {});
        QVERIFY(role(m, 0, MailList::SeenRole).toBool());
        QVERIFY(!role(m, 0, MailList::FlaggedRole).toBool());
    }
    void rich_roles_escaped()
    {
        MailList m;
        m.setRows({mk(1, true, "<img src=x>")});
        QVERIFY(!role(m, 0, MailList::SubjectRichRole).toString().contains("<img"));
        m.setWords({"img"}, QColor(255, 200, 0));
        QVERIFY(!role(m, 0, MailList::SubjectRichRole).toString().contains("<img"));
    }
};

QTEST_MAIN(TstMailList)
#include "tst_maillist.moc"
