/* OBS Studio status bar viewer count. GPL-2.0-or-later. */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/bmem.h>
#include "counter-core.hpp"
#include "winhttp-client.hpp"
#include <QAction>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QInputDialog>
#include <QMessageBox>
#include <QLineEdit>
#include <QPointer>
#include <QSettings>
#include <QThread>
#include <QTimer>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-fixed-counter", "en-US")

namespace {
constexpr auto DefaultChannel = "UCQXPKDYp4U9JkTWAON_AhOw";

class FetchThread final : public QThread {
    counter::Target target;
public:
    std::atomic_bool cancelled{false};
    const quint64 generation;
    counter::Result result;
    FetchThread(counter::Target selected, quint64 version, QObject *parent)
        : QThread(parent), target(selected), generation(version) {}
protected:
    void run() override
    {
        const auto reply = native_http::fetch(target.url, cancelled);
        if (!reply.error.isEmpty()) result = {{}, QStringLiteral("Citanje YouTube-a nije uspelo: ") + reply.error};
        else result = counter::parse(QString::fromUtf8(reply.body), target.video, target.channel);
    }
};

class Controller final : public QObject {
    QMainWindow *main;
    QTimer pollTimer, uiTimer;
    QPointer<FetchThread> pending;
    QPointer<QLabel> label;
    QPointer<QAction> action;
    counter::Target selected;
    QString source, configPath, number, status;
    qint64 updated = 0;
    bool warnedLayout = false;
    quint64 generation = 0;
    bool refreshAfterCancel = false;
public:
    explicit Controller(QMainWindow *window) : QObject(window), main(window)
    {
        char *path = obs_module_config_path("settings.ini");
        if (path) {
            configPath = QString::fromUtf8(path);
            bfree(path);
            QDir().mkpath(QFileInfo(configPath).absolutePath());
        }
        source = QString::fromLatin1(DefaultChannel);
        if (!configPath.isEmpty()) source = QSettings(configPath, QSettings::IniFormat).value(QStringLiteral("source"), source).toString();
        selected = counter::target(source);
        if (selected.url.isEmpty()) { source = QString::fromLatin1(DefaultChannel); selected = counter::target(source); }
        action = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction("Broj gledalaca - kanal"));
        if (action) QObject::connect(action.data(), &QAction::triggered, this, [this]() { settings(); });
        QObject::connect(&pollTimer, &QTimer::timeout, this, [this]() { request(); });
        QObject::connect(&uiTimer, &QTimer::timeout, this, [this]() { attach(); paint(); });
        pollTimer.start(5000);
        uiTimer.start(1000);
        attach();
        request();
    }

    ~Controller() override
    {
        pollTimer.stop(); uiTimer.stop();
        if (pending) {
            QObject::disconnect(pending.data(), nullptr, this, nullptr);
            pending->cancelled.store(true);
            // Cancellation wakes the worker within 100ms. Its native request
            // must finish closing before the DLL can safely be unloaded.
            pending->wait();
            delete pending.data();
            pending.clear();
        }
        delete label.data();
        delete action.data();
    }

    void attach()
    {
        if (label) return;
        auto *layout = counter::statusLayout(main);
        if (!layout) {
            if (!warnedLayout) blog(LOG_WARNING, "[Center viewers] Status bar layout was not recognized; no UI was modified.");
            warnedLayout = true;
            return;
        }
        label = counter::addStatusLabel(layout);
        if (label) blog(LOG_INFO, "[Center viewers] Counter inserted at the left of the status bar.");
        paint();
    }

    void paint()
    {
        if (!label) return;
        const bool fresh = updated > 0 && QDateTime::currentMSecsSinceEpoch() - updated < 15000;
        label->setText(fresh && !number.isEmpty() ? number : QStringLiteral("\u2014"));
        counter::syncStatusFont(label);
    }

    void publish(counter::Result result)
    {
        number = result.number;
        if (status != result.status) {
            const auto message = result.status.toUtf8();
            blog(LOG_INFO, "[Center viewers] %s", message.constData());
        }
        status = result.status;
        updated = QDateTime::currentMSecsSinceEpoch();
        paint();
    }

    void request()
    {
        if (pending || selected.url.isEmpty()) return;
        auto *worker = new FetchThread(selected, generation, this);
        pending = worker;
        QObject::connect(worker, &QThread::finished, this, [this, worker]() {
            pending.clear();
            if (worker->generation == generation) publish(worker->result);
            worker->deleteLater();
            if (refreshAfterCancel) { refreshAfterCancel = false; request(); }
        });
        worker->start(QThread::LowPriority);
    }

    void settings()
    {
        bool ok = false;
        const QString value = QInputDialog::getText(main, QStringLiteral("Broj gledalaca"),
            QStringLiteral("YouTube kanal (@ime ili Channel ID) ili link prenosa:\nBroj se proverava svakih 5 sekundi."),
            QLineEdit::Normal, source, &ok).trimmed();
        if (!ok || value == source) return;
        const auto target = counter::target(value);
        if (target.url.isEmpty()) { QMessageBox::warning(main, QStringLiteral("Broj gledalaca"), QStringLiteral("Unesi ispravan YouTube kanal ili link prenosa.")); return; }
        if (pending) {
            pending->cancelled.store(true);
            refreshAfterCancel = true;
        }
        ++generation;
        source = value; selected = target;
        if (!configPath.isEmpty()) QSettings(configPath, QSettings::IniFormat).setValue(QStringLiteral("source"), source);
        publish({{}, QStringLiteral("Citanje novog kanala...")});
        request();
    }
};

QPointer<Controller> controller;
bool exiting = false;

void start()
{
    if (controller || exiting) return;
    auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    if (main) controller = new Controller(main);
}

void onEvent(enum obs_frontend_event event, void *)
{
    if (event == OBS_FRONTEND_EVENT_EXIT) {
        exiting = true;
        delete controller.data();
        controller.clear();
        return;
    }
    if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
        auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
        if (main) QTimer::singleShot(0, main, []() { start(); });
    } else if (event == OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED && controller) {
        QTimer::singleShot(0, controller.data(), []() { if (controller) controller->attach(); });
    }
}
} // namespace

extern "C" const char *obs_module_description(void)
{
    return "Live YouTube viewers or waiting viewers at the left of the OBS status bar.";
}

extern "C" bool obs_module_load(void)
{
    obs_frontend_add_event_callback(onEvent, nullptr);
    blog(LOG_INFO, "[Center viewers] Version 2.2 loaded; HTTPS uses Windows WinHTTP/Schannel (no Qt TLS backend).");
    return true;
}

extern "C" void obs_module_unload(void)
{
    obs_frontend_remove_event_callback(onEvent, nullptr);
    delete controller.data(); controller.clear();
}
