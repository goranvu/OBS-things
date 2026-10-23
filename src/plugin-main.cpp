/* OBS Studio center viewer count. GPL-2.0-or-later. */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/bmem.h>
#include "counter-core.hpp"
#include <QAction>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QInputDialog>
#include <QMessageBox>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSettings>
#include <QTimer>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-fixed-counter", "en-US")

namespace {
constexpr auto DefaultChannel = "UCQXPKDYp4U9JkTWAON_AhOw";
constexpr qint64 MaxResponse = 8 * 1024 * 1024;

class Controller final : public QObject {
    QMainWindow *main;
    QNetworkAccessManager network;
    QTimer pollTimer, uiTimer;
    QPointer<QNetworkReply> pending;
    QPointer<QLabel> label;
    QPointer<QAction> action;
    counter::Target selected;
    QString source, configPath, number, status;
    qint64 updated = 0;
    bool warnedLayout = false;
public:
    explicit Controller(QMainWindow *window) : QObject(window), main(window), network(this)
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
            pending->abort();
        }
        delete label.data();
        delete action.data();
    }

    void attach()
    {
        if (!obs_frontend_preview_program_mode_active() || label) return;
        auto *layout = counter::centerLayout(main);
        if (!layout) {
            if (!warnedLayout) blog(LOG_WARNING, "[Center viewers] Center transition layout was not recognized; no UI was modified.");
            warnedLayout = true;
            return;
        }
        label = counter::addLabel(layout);
        if (label) blog(LOG_INFO, "[Center viewers] Counter inserted below Studio Mode transitions.");
        paint();
    }

    void paint()
    {
        if (!label) return;
        const bool fresh = updated > 0 && QDateTime::currentMSecsSinceEpoch() - updated < 15000;
        label->setText(fresh && !number.isEmpty() ? number : QStringLiteral("\u2014"));
        label->setToolTip(status + QStringLiteral("\n") + source);
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
        QNetworkRequest request(selected.url);
        request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Mozilla/5.0 OBSViewerCounter/2.0"));
        request.setRawHeader("Accept-Language", "en-US,en;q=0.9");
        request.setRawHeader("Cache-Control", "no-cache");
        request.setRawHeader("Pragma", "no-cache");
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        request.setTransferTimeout(8000);
        auto *reply = network.get(request);
        pending = reply;
        const counter::Target requested = selected;
        // The transfer timeout measures idle time; this is an absolute deadline.
        QTimer::singleShot(8000, reply, [reply]() { if (reply->isRunning()) reply->abort(); });
        QObject::connect(reply, &QNetworkReply::readyRead, this, [reply]() {
            if (reply->bytesAvailable() > MaxResponse) reply->abort();
        });
        QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, requested]() {
            pending.clear();
            const QString host = reply->url().host().toLower();
            const auto body = reply->read(MaxResponse + 1);
            if (reply->error() != QNetworkReply::NoError) publish({{}, QStringLiteral("Citanje YouTube-a nije uspelo: ") + reply->errorString()});
            else if (host != QStringLiteral("www.youtube.com") && host != QStringLiteral("youtube.com") && host != QStringLiteral("m.youtube.com"))
                publish({{}, QStringLiteral("YouTube trazi saglasnost ili prijavu.")});
            else if (body.size() > MaxResponse) publish({{}, QStringLiteral("YouTube odgovor je prevelik.")});
            else publish(counter::parse(QString::fromUtf8(body), requested.video, requested.channel));
            reply->deleteLater();
        });
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
            QObject::disconnect(pending.data(), nullptr, this, nullptr);
            pending->abort(); pending->deleteLater(); pending.clear();
        }
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
    return "Live YouTube viewers or waiting viewers below Studio Mode transition controls.";
}

extern "C" bool obs_module_load(void)
{
    obs_frontend_add_event_callback(onEvent, nullptr);
    blog(LOG_INFO, "[Center viewers] Plugin loaded; awaiting OBS frontend.");
    return true;
}

extern "C" void obs_module_unload(void)
{
    obs_frontend_remove_event_callback(onEvent, nullptr);
    delete controller.data(); controller.clear();
}
