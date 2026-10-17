#pragma once
// One window per system: the first omarchy-mail listens on $XDG_RUNTIME_DIR/omarchy-mail-window, a later one hands
// its arguments over (one JSON array per connection) and exits.
#include <QLocalServer>
#include <QStringList>

namespace om::single {

bool forward(const QStringList &args);   // true: a running window took them

class Server : public QObject {
    Q_OBJECT
public:
    bool listen();   // false: could not listen; the window then just opens
Q_SIGNALS:
    void received(const QStringList &args);

private:
    QLocalServer m_server;
};

} // namespace om::single
