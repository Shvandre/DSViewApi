#ifndef DSVIEW_PV_REMOTESERVER_H
#define DSVIEW_PV_REMOTESERVER_H

#include <QThread>
#include <QObject>
#include <atomic>

namespace pv {

class SigSession;
class StoreSession;

class RemoteServer : public QThread
{
    Q_OBJECT

public:
    explicit RemoteServer(SigSession *session, int port = 8321);
    ~RemoteServer();

    void shutdown();

protected:
    void run() override;

private:
    QString handleCommand(const QString &cmd);

    SigSession *_session;
    int _port;
    std::atomic<bool> _running;
    int _server_fd;
};

} // namespace pv

#endif // DSVIEW_PV_REMOTESERVER_H
