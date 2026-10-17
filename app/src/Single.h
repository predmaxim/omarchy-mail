#pragma once
// One window per system: the first omarchy-mail listens on $XDG_RUNTIME_DIR/omarchy-mail-window; a later one hands
// its arguments over (one line {"args":[...]} per connection) and exits.
#include <QLocalServer>
#include <QStringList>

namespace om::single {

class Server : public QObject {
    Q_OBJECT
public:
    enum Claim {
        Listening,   // this process is the window
        Forwarded,   // a live window took the arguments
        Unreachable, // the name is taken by a window that does not answer: do not steal it, do not open another
    };
    // Called before anything heavy is built. Only a dead socket (connection refused) is replaced.
    Claim claim(const QStringList &args);
Q_SIGNALS:
    void received(const QStringList &args);   // only well-formed lines

private:
    QLocalServer m_server;
};

} // namespace om::single
