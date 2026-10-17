// copied from omarchy-quicklook (own MIT code)
#pragma once
// Omarchy theme: palette, font size, icon theme (spec: "Тема Omarchy", "Общее").
#include <QColor>
#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>

namespace om {

class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QColor background READ background NOTIFY changed)
    Q_PROPERTY(QColor foreground READ foreground NOTIFY changed)
    Q_PROPERTY(QColor accent READ accent NOTIFY changed)
    Q_PROPERTY(QColor muted READ muted NOTIFY changed)
    Q_PROPERTY(QColor urgent READ urgent NOTIFY changed)
    Q_PROPERTY(QColor dim READ dim NOTIFY changed)
    Q_PROPERTY(QColor mark READ mark NOTIFY changed)
    Q_PROPERTY(QColor selection READ selection NOTIFY changed)
    Q_PROPERTY(QColor hoverFill READ hoverFill NOTIFY changed)
    Q_PROPERTY(QColor currentFill READ currentFill NOTIFY changed)
    Q_PROPERTY(QColor cursorBorder READ cursorBorder NOTIFY changed)
    Q_PROPERTY(int baseSize READ baseSize NOTIFY changed)
    Q_PROPERTY(int fontCaption READ fontCaption NOTIFY changed)
    Q_PROPERTY(int fontBody READ fontBody NOTIFY changed)
    Q_PROPERTY(int fontSubtitle READ fontSubtitle NOTIFY changed)
    Q_PROPERTY(int fontTitle READ fontTitle NOTIFY changed)
    Q_PROPERTY(int fontHeading READ fontHeading NOTIFY changed)
    Q_PROPERTY(int fontDisplay READ fontDisplay NOTIFY changed)
    Q_PROPERTY(int iconLarge READ iconLarge NOTIFY changed)
    Q_PROPERTY(QString iconTheme READ iconTheme NOTIFY changed)
public:
    explicit Theme(QObject *parent = nullptr);   // calls reload() and starts watching

    void reload();          // `omarchy-theme-color --all` (fallback: colors.toml), base-size, icons.theme
    QHash<QString, QColor> palette() const { return m_palette; }
    QColor background() const;
    QColor foreground() const;
    QColor accent() const;
    QColor muted() const;
    int baseSize() const { return m_baseSize; }
    QColor urgent() const;      // red -> color1 -> #a55555
    QColor dim() const;         // foreground.darker(140)
    QColor mark() const;        // yellow -> color3 -> accent (search highlight)
    QColor selection() const;   // selection -> accent
    QColor hoverFill() const;     // foreground, alpha 0.08
    QColor currentFill() const;   // foreground, alpha 0.18
    QColor cursorBorder() const;  // foreground, alpha 0.25
    int fontCaption() const { return scaled(0.833); }
    int fontBody() const { return scaled(1.0); }
    int fontSubtitle() const { return scaled(1.083); }
    int fontTitle() const { return scaled(1.167); }
    int fontHeading() const { return scaled(1.333); }
    int fontDisplay() const { return scaled(2.0); }
    int iconLarge() const { return scaled(1.5); }
    Q_INVOKABLE int space(int px) const;   // max(1, round(px * base / 12))
    QString iconTheme() const { return m_iconTheme; }

    static QHash<QString, QColor> parsePalette(const QByteArray &tsv);       // "key<TAB>#rrggbb" per line
    static QHash<QString, QColor> parseColorsToml(const QByteArray &toml);   // key = "#rrggbb"; adds muted -> color8 -> foreground
    static int parseBaseSize(const QByteArray &toml, int fallback);          // [font] base-size = N
    static QString currentDir();      // $XDG_STATE_HOME or ~/.local/state, + /omarchy/current
    static QString userShellToml();   // $XDG_CONFIG_HOME or ~/.config, + /omarchy/shell.toml

Q_SIGNALS:
    void changed();

private:
    int scaled(double f) const;
    QColor fill(double alpha) const;
    void rewatch();   // re-add theme.name, user shell.toml and ~/.config/omarchy (atomic writes drop watches)

    QHash<QString, QColor> m_palette;
    int m_baseSize = 16;
    QString m_iconTheme;
    QFileSystemWatcher m_watcher;
};

} // namespace om
