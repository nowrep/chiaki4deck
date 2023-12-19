#include "qmlbackend.h"
#include "qmlmainwindow.h"
#include "streamsession.h"
#include "loginpindialog.h"
#include "settingsdialog.h"
#include "registdialog.h"
#include "controllermanager.h"

#include <QGuiApplication>

QmlController::QmlController(Controller *c, QObject *t, QObject *parent)
    : QObject(parent)
    , target(t)
    , controller(c)
{
    connect(controller, &Controller::StateChanged, this, [this]() {
        static QVector<QPair<uint32_t, Qt::Key>> key_map = {
            { CHIAKI_CONTROLLER_BUTTON_DPAD_UP, Qt::Key_Up },
            { CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN, Qt::Key_Down },
            { CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT, Qt::Key_Left },
            { CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT, Qt::Key_Right },
            { CHIAKI_CONTROLLER_BUTTON_CROSS, Qt::Key_Space },
            { CHIAKI_CONTROLLER_BUTTON_MOON, Qt::Key_Escape },
        };
        uint32_t buttons = controller->GetState().buttons;
        for (auto &k : key_map) {
            const bool pressed = buttons & k.first;
            const bool old_pressed = old_buttons & k.first;
            if (pressed && !old_pressed) {
                QKeyEvent *ev = new QKeyEvent(QEvent::KeyPress, k.second, Qt::NoModifier);
                QGuiApplication::postEvent(target, ev);
            } else if (!pressed && old_pressed) {
                QKeyEvent *ev = new QKeyEvent(QEvent::KeyRelease, k.second, Qt::NoModifier);
                QGuiApplication::postEvent(target, ev);
            }
        }
        old_buttons = buttons;
    });
}

QmlBackend::QmlBackend(Settings *settings, QmlMainWindow *window)
    : QObject(window)
    , settings(settings)
    , main_window(window)
{
    const char *uri = "org.streetpea.chiaki4deck";
    qmlRegisterSingletonInstance(uri, 1, 0, "Chiaki", this);
    qmlRegisterUncreatableType<QmlMainWindow>(uri, 1, 0, "ChiakiWindow", {});
    qmlRegisterUncreatableType<StreamSession>(uri, 1, 0, "ChiakiSession", {});

    QObject *frame_obj = new QObject();
    frame_thread = new QThread(frame_obj);
    frame_thread->setObjectName("frame");
    frame_thread->start();
    frame_obj->moveToThread(frame_thread);

    connect(settings, &Settings::RegisteredHostsUpdated, this, &QmlBackend::hostsChanged);
    connect(settings, &Settings::ManualHostsUpdated, this, &QmlBackend::hostsChanged);
    connect(&discovery_manager, &DiscoveryManager::HostsUpdated, this, &QmlBackend::hostsChanged);
    setDiscoveryEnabled(discoveryEnabled());

    connect(ControllerManager::GetInstance(), &ControllerManager::AvailableControllersUpdated, this, &QmlBackend::updateControllers);
    updateControllers();
}

QmlBackend::~QmlBackend()
{
    frame_thread->quit();
    frame_thread->wait();
    delete frame_thread->parent();
}

QmlMainWindow *QmlBackend::window() const
{
    return main_window;
}

StreamSession *QmlBackend::session() const
{
    return stream_session;
}

bool QmlBackend::discoveryEnabled() const
{
    return settings->GetDiscoveryEnabled();
}

void QmlBackend::setDiscoveryEnabled(bool enabled)
{
    settings->SetDiscoveryEnabled(enabled);
    discovery_manager.SetActive(enabled);
    emit discoveryEnabledChanged();
}

QVariantList QmlBackend::hosts() const
{
    QVariantList out;
    for (const auto &host : discovery_manager.GetHosts()) {
        QVariantMap m;
        m["discovered"] = true;
        m["manual"] = false;
        m["name"] = host.host_name;
        m["address"] = host.host_addr;
        m["ps5"] = host.ps5;
        m["mac"] = host.GetHostMAC().ToString();
        m["state"] = chiaki_discovery_host_state_string(host.state);
        m["app"] = host.running_app_name;
        m["titleId"] = host.running_app_titleid;
        m["registered"] = settings->GetRegisteredHostRegistered(host.GetHostMAC());
        out.append(m);
    }
    for (const auto &host : settings->GetManualHosts()) {
        QVariantMap m;
        m["discovered"] = false;
        m["manual"] = true;
        m["name"] = host.GetHost();
        m["address"] = host.GetHost();
        m["registered"] = false;
        if (host.GetRegistered() && settings->GetRegisteredHostRegistered(host.GetMAC())) {
            auto registered = settings->GetRegisteredHost(host.GetMAC());
            m["registered"] = true;
            m["name"] = registered.GetServerNickname();
            m["ps5"] = chiaki_target_is_ps5(registered.GetTarget());
            m["mac"] = registered.GetServerMAC().ToString();
        }
        out.append(m);
    }
    return out;
}

void QmlBackend::createSession(const StreamSessionConnectInfo &connect_info)
{
    if (stream_session) {
        qCWarning(chiakiGui) << "Another session is already active";
        return;
    }

    qDeleteAll(controllers);
    controllers.clear();

    try {
        stream_session = new StreamSession(connect_info, this);
    } catch(const Exception &e) {
        emit error(tr("Stream failed"), tr("Failed to initialize Stream Session: %1").arg(e.what()));
        updateControllers();
        return;
    }

    connect(stream_session, &StreamSession::FfmpegFrameAvailable, frame_thread->parent(), [this]() {
        ChiakiFfmpegDecoder *decoder = stream_session->GetFfmpegDecoder();
        if (!decoder) {
            qCCritical(chiakiGui) << "Session has no FFmpeg decoder";
            return;
        }
        AVFrame *frame = chiaki_ffmpeg_decoder_pull_frame(decoder, /*hw_download*/ false);
        if (frame)
            QMetaObject::invokeMethod(main_window, std::bind(&QmlMainWindow::presentFrame, main_window, frame));
    });

    connect(stream_session, &StreamSession::SessionQuit, this, [this](ChiakiQuitReason reason, const QString &reason_str) {
        if (chiaki_quit_reason_is_error(reason)) {
            QString m = tr("Chiaki Session has quit") + ":\n" + chiaki_quit_reason_string(reason);
            if (!reason_str.isEmpty())
                m += "\n" + tr("Reason") + ": \"" + reason_str + "\"";
            emit sessionError(tr("Session has quit"), m);
        }

        qDeleteAll(controllers);
        controllers.clear();
        connect(stream_session, &QObject::destroyed, this, [this]() {
            stream_session = nullptr;
            updateControllers();
            emit sessionChanged(stream_session);
        });
        stream_session->deleteLater();
    });

    connect(stream_session, &StreamSession::LoginPINRequested, this, [this, connect_info](bool incorrect) {
        if (!connect_info.initial_login_pin.isEmpty() && incorrect == false)
            stream_session->SetLoginPIN(connect_info.initial_login_pin);
        else
            showLoginPINDialog(incorrect);
    });

    if (connect_info.fullscreen || connect_info.zoom || connect_info.stretch)
        main_window->showFullScreen();
    else
        main_window->resize(connect_info.video_profile.width, connect_info.video_profile.height);

    updateControllers();

    stream_session->Start();
    emit sessionChanged(stream_session);
}

bool QmlBackend::closeRequested()
{
    if (!stream_session)
        return true;

    bool stop = true;
    if (stream_session->IsConnected()) {
        switch (settings->GetDisconnectAction()) {
        case DisconnectAction::Ask:
            stop = false;
            emit sessionStopDialogRequested();
            break;
        case DisconnectAction::AlwaysSleep:
            stream_session->GoToBed();
            break;
        default:
            break;
        }
    }

    if (stop)
        stream_session->Stop();

    return false;
}

void QmlBackend::deleteHost(int index)
{
    auto server = displayServerAt(index);
    if (!server.valid || server.discovered)
        return;
    settings->RemoveManualHost(server.manual_host.GetID());
}

void QmlBackend::wakeUpHost(int index)
{
    auto server = displayServerAt(index);
    if (!server.valid)
        return;
    sendWakeup(server);
}

void QmlBackend::addManualHost(int index, const QString &address)
{
    HostMAC hmac;
    if (index >= 0) {
        auto server = displayServerAt(index);
        if (!server.valid)
            return;
        hmac = server.registered_host.GetServerMAC();
    }
    ManualHost host(-1, address, index >= 0, hmac);
    settings->SetManualHost(host);
}

void QmlBackend::connectToHost(int index)
{
    auto server = displayServerAt(index);
    if (!server.valid)
        return;

    if (!server.registered) {
        RegistDialog regist_dialog(settings, server.GetHostAddr());
        int r = regist_dialog.exec();
        if (r == QDialog::Accepted && !server.discovered) { // success
            ManualHost manual_host = server.manual_host;
            manual_host.Register(regist_dialog.GetRegisteredHost());
            settings->SetManualHost(manual_host);
        }
        return;
    }

    // Need to wake console first
    if (server.discovered && server.discovery_host.state == CHIAKI_DISCOVERY_HOST_STATE_STANDBY)
        return;

    QString host = server.GetHostAddr();
    StreamSessionConnectInfo info(
            settings,
            server.registered_host.GetTarget(),
            host,
            server.registered_host.GetRPRegistKey(),
            server.registered_host.GetRPKey(),
            {},
            false,
            false,
            false);
    createSession(info);
}

void QmlBackend::stopSession(bool sleep)
{
    if (!stream_session)
        return;

    if (sleep)
        stream_session->GoToBed();

    stream_session->Stop();
}

void QmlBackend::showSettingsDialog()
{
    SettingsDialog dialog(settings);
    dialog.exec();
}

QmlBackend::DisplayServer QmlBackend::displayServerAt(int index) const
{
    if (index < 0)
        return {};
    auto discovered = discovery_manager.GetHosts();
    if (index < discovered.size()) {
        DisplayServer server;
        server.valid = true;
        server.discovered = true;
        server.discovery_host = discovered.at(index);
        server.registered = settings->GetRegisteredHostRegistered(server.discovery_host.GetHostMAC());
        if (server.registered)
            server.registered_host = settings->GetRegisteredHost(server.discovery_host.GetHostMAC());
        return server;
    }
    index -= discovered.size();
    auto manual = settings->GetManualHosts();
    if (index < manual.size()) {
        DisplayServer server;
        server.valid = true;
        server.discovered = false;
        server.manual_host = manual.at(index);
        server.registered = false;
        if (server.manual_host.GetRegistered() && settings->GetRegisteredHostRegistered(server.manual_host.GetMAC())) {
            server.registered = true;
            server.registered_host = settings->GetRegisteredHost(server.manual_host.GetMAC());
        }
        return server;
    }
    return {};
}

void QmlBackend::sendWakeup(const DisplayServer &server)
{
    if (!server.registered)
        return;
    try {
        discovery_manager.SendWakeup(server.GetHostAddr(), server.registered_host.GetRPRegistKey(),
                chiaki_target_is_ps5(server.registered_host.GetTarget()));
    } catch(const Exception &e) {
        emit error(tr("Wakeup failed"), tr("Failed to send Wakeup packet:\n%1").arg(e.what()));
    }
}

void QmlBackend::showLoginPINDialog(bool incorrect)
{
    auto dialog = new LoginPINDialog(incorrect);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::finished, this, [this, dialog](int result) {
        if (!stream_session)
            return;
        if (result == QDialog::Accepted)
            stream_session->SetLoginPIN(dialog->GetPIN());
        else
            stream_session->Stop();
    });
    dialog->show();
}

void QmlBackend::updateControllers()
{
    if (stream_session) {
        for (Controller *controller : stream_session->GetControllers()) {
            if (controllers.contains(controller->GetDeviceID()))
                continue;
            controllers[controller->GetDeviceID()] = new QmlController(controller, main_window->quick_window, this);
        }
    } else {
        for (auto id : ControllerManager::GetInstance()->GetAvailableControllers()) {
            if (controllers.contains(id))
                continue;
            auto controller = ControllerManager::GetInstance()->OpenController(id);
            if (!controller)
                continue;
            controllers[id] = new QmlController(controller, main_window->quick_window, this);
            controller->setParent(controllers[id]);
        }
    }
}