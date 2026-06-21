#include "remoteserver.h"
#include "sigsession.h"
#include "storesession.h"
#include "deviceagent.h"
#include "view/logicsignal.h"
#include "config/appconfig.h"
#include "log.h"

#include <libsigrok.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>

#include <QFileInfo>
#include <QStringList>

namespace pv {

// Map a LogicSignal trigger enum value to a human string and back.
static const char *trig_to_str(int trig)
{
    switch (trig) {
    case view::LogicSignal::NONTRIG: return "none";
    case view::LogicSignal::POSTRIG: return "rising";
    case view::LogicSignal::HIGTRIG: return "high";
    case view::LogicSignal::NEGTRIG: return "falling";
    case view::LogicSignal::LOWTRIG: return "low";
    case view::LogicSignal::EDGTRIG: return "edge";
    default: return "?";
    }
}

static int str_to_trig(const QString &s)
{
    QString t = s.toLower();
    if (t == "none")    return view::LogicSignal::NONTRIG;
    if (t == "rising")  return view::LogicSignal::POSTRIG;
    if (t == "high")    return view::LogicSignal::HIGTRIG;
    if (t == "falling") return view::LogicSignal::NEGTRIG;
    if (t == "low")     return view::LogicSignal::LOWTRIG;
    if (t == "edge")    return view::LogicSignal::EDGTRIG;
    return -1;
}

RemoteServer::RemoteServer(SigSession *session, int port)
    : _session(session)
    , _port(port)
    , _running(false)
    , _server_fd(-1)
{
}

RemoteServer::~RemoteServer()
{
    shutdown();
}

void RemoteServer::shutdown()
{
    _running = false;
    if (_server_fd >= 0) {
        ::close(_server_fd);
        _server_fd = -1;
    }
    if (isRunning())
        wait(2000);
}

void RemoteServer::run()
{
    // Avoid the modal "trigger set on multiple channels" warning blocking
    // headless automation. Runtime-only; not persisted (no SaveApp()).
    AppConfig::Instance().appOptions.warnofMultiTrig = false;

    _server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (_server_fd < 0) {
        dsv_err("RemoteServer: socket() failed");
        return;
    }

    int opt = 1;
    setsockopt(_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(_port);

    if (bind(_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        dsv_err("RemoteServer: bind() failed on port %d", _port);
        ::close(_server_fd);
        _server_fd = -1;
        return;
    }

    if (listen(_server_fd, 4) < 0) {
        dsv_err("RemoteServer: listen() failed");
        ::close(_server_fd);
        _server_fd = -1;
        return;
    }

    dsv_info("RemoteServer: listening on 127.0.0.1:%d", _port);
    _running = true;

    while (_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(_server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (_running)
                dsv_err("RemoteServer: accept() failed");
            break;
        }

        char buf[1024];
        memset(buf, 0, sizeof(buf));
        ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            QString cmd = QString::fromUtf8(buf, n).trimmed();
            QString response = handleCommand(cmd);
            QByteArray resp_bytes = response.toUtf8();
            send(client_fd, resp_bytes.constData(), resp_bytes.size(), 0);
        }

        ::close(client_fd);
    }
}

QString RemoteServer::handleCommand(const QString &cmd)
{
    QStringList parts = cmd.split(' ', Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return "ERROR: empty command\n";

    QString action = parts[0].toLower();

    if (action == "start") {
        if (_session->is_working())
            return "ERROR: already running\n";

        // Commit the simple trigger synchronously before arming. The GUI does
        // this via try_commit_trigger() on DSV_MSG_START_COLLECT_WORK_PREV, but
        // that message is delivered to the main thread asynchronously (queued)
        // when start_capture() is called from this server thread, racing the
        // FPGA arm. Replicate the simple-trigger commit here so it is in place
        // before exec_capture() reads it. Idempotent: the later main-thread
        // try_commit_trigger() re-commits the same per-channel signal states.
        if (_session->get_device()->get_work_mode() == LOGIC
                && !_session->is_instant()) {
            ds_trigger_reset();
            for (auto s : _session->get_signals()) {
                if (s->signal_type() == SR_CHANNEL_LOGIC)
                    ((view::LogicSignal *)s)->commit_trig();
            }
        }

        bool ok = _session->start_capture(false);
        return ok ? "OK\n" : "ERROR: start failed\n";

    } else if (action == "stop") {
        if (!_session->is_working())
            return "OK: not running\n";
        _session->stop_capture();
        return "OK\n";

    } else if (action == "status") {
        if (_session->is_running_status())
            return "RUNNING\n";
        else if (_session->is_stopped_status())
            return "STOPPED\n";
        else
            return "IDLE\n";

    } else if (action == "threshold") {
        if (parts.size() < 2)
            return "ERROR: usage: threshold <voltage>, e.g. threshold 1.4\n";

        bool ok = false;
        double vth = parts[1].toDouble(&ok);
        if (!ok || vth < 0.0 || vth > 5.0)
            return "ERROR: voltage must be 0.0-5.0\n";

        if (_session->is_working())
            return "ERROR: cannot change threshold during capture\n";

        DeviceAgent *dev = _session->get_device();
        if (!dev)
            return "ERROR: no device\n";

        ok = dev->set_config_double(SR_CONF_VTH, vth);
        if (ok)
            return "OK: threshold set to " + QString::number(vth, 'f', 2) + "V\n";
        return "ERROR: failed to set threshold\n";

    } else if (action == "trigger") {
        // No args: report current simple trigger on every logic channel.
        if (parts.size() == 1) {
            QString out;
            for (auto s : _session->get_signals()) {
                if (s->signal_type() != SR_CHANNEL_LOGIC)
                    continue;
                view::LogicSignal *ls = (view::LogicSignal *)s;
                out += "ch" + QString::number(ls->get_index()) + " "
                     + trig_to_str(ls->get_trig()) + "\n";
            }
            if (out.isEmpty())
                return "ERROR: no logic channels\n";
            return out;
        }

        // Set: trigger <channel> <type>
        if (parts.size() < 3)
            return "ERROR: usage: trigger <channel> <none|rising|falling|high|low|edge>\n";

        if (_session->is_working())
            return "ERROR: cannot change trigger during capture\n";

        bool ok = false;
        int ch = parts[1].toInt(&ok);
        if (!ok)
            return "ERROR: invalid channel\n";

        int trig = str_to_trig(parts[2]);
        if (trig < 0)
            return "ERROR: type must be none|rising|falling|high|low|edge\n";

        for (auto s : _session->get_signals()) {
            if (s->signal_type() != SR_CHANNEL_LOGIC)
                continue;
            view::LogicSignal *ls = (view::LogicSignal *)s;
            if (ls->get_index() == ch) {
                ls->set_trig(trig);
                return "OK: ch" + QString::number(ch) + " trigger = "
                     + parts[2].toLower() + " (applied on next start)\n";
            }
        }
        return "ERROR: channel " + QString::number(ch) + " not found\n";

    } else if (action == "export") {
        if (parts.size() < 2)
            return "ERROR: usage: export <filename.csv>\n";

        if (_session->is_working())
            return "ERROR: capture still running\n";

        QString filename = parts[1];
        QFileInfo fi(filename);
        QString suffix = fi.suffix().toLower();

        StoreSession ss(_session);

        if (ss.is_busy())
            return "ERROR: export already in progress\n";

        ss.SetDataRange(0, 0);
        ss.setExportFile(filename, suffix);

        bool ok = ss.export_start();
        if (ok) {
            ss.wait();
            return "OK: exported to " + filename + "\n";
        }
        return "ERROR: export failed: " + ss.error() + "\n";

    } else if (action == "ping") {
        return "PONG\n";

    } else {
        return "ERROR: unknown command. Available: start, stop, status, "
               "threshold <V>, trigger [<ch> <type>], export <file>, ping\n";
    }
}

} // namespace pv
