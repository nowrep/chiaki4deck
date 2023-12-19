#pragma once

#include "streamsession.h"
#include "discoverymanager.h"

#include <QObject>
#include <QThread>

class QmlMainWindow;
class StreamSession;

class QmlController : public QObject
{
public:
    QmlController(Controller *controller, QObject *target, QObject *parent = nullptr);

private:
    QObject *target = {};
    uint32_t old_buttons = 0;
    Controller *controller = {};
};

class QmlBackend : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QmlMainWindow* window READ window CONSTANT)
    Q_PROPERTY(StreamSession* session READ session NOTIFY sessionChanged)
    Q_PROPERTY(bool discoveryEnabled READ discoveryEnabled WRITE setDiscoveryEnabled NOTIFY discoveryEnabledChanged)
    Q_PROPERTY(QVariantList hosts READ hosts NOTIFY hostsChanged)

public:
    QmlBackend(Settings *settings, QmlMainWindow *window);
    ~QmlBackend();

    QmlMainWindow *window() const;
    StreamSession *session() const;

    bool discoveryEnabled() const;
    void setDiscoveryEnabled(bool enabled);

    QVariantList hosts() const;

    void createSession(const StreamSessionConnectInfo &connect_info);

    bool closeRequested();

    Q_INVOKABLE void deleteHost(int index);
    Q_INVOKABLE void wakeUpHost(int index);
    Q_INVOKABLE void addManualHost(int index, const QString &address);
    Q_INVOKABLE void connectToHost(int index);
    Q_INVOKABLE void stopSession(bool sleep);

    Q_INVOKABLE void showSettingsDialog();

signals:
    void sessionChanged(StreamSession *session);
    void discoveryEnabledChanged();
    void hostsChanged();

    void error(const QString &title, const QString &text);
    void sessionError(const QString &title, const QString &text);
    void sessionStopDialogRequested();

private:
    struct DisplayServer {
        bool valid = false;

        DiscoveryHost discovery_host;
        ManualHost manual_host;
        bool discovered;

        RegisteredHost registered_host;
        bool registered;

        QString GetHostAddr() const { return discovered ? discovery_host.host_addr : manual_host.GetHost(); }
        bool IsPS5() const { return discovered ? discovery_host.ps5 :
            (registered ? chiaki_target_is_ps5(registered_host.GetTarget()) : false); }
    };

    DisplayServer displayServerAt(int index) const;
    void sendWakeup(const DisplayServer &server);
    void showLoginPINDialog(bool incorrect);
    void updateControllers();

    Settings *settings = {};
    QmlMainWindow *main_window = {};
    StreamSession *stream_session = {};
    QThread *frame_thread = {};
    DiscoveryManager discovery_manager;
    QHash<int, QmlController*> controllers;
};