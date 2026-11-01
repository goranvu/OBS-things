#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QDockWidget>
#include <QLineEdit>
#include <QMainWindow>
#include <QPointer>
#include <QVBoxLayout>
#include <QWidget>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-edit-panel", "en-US")

static QPointer<QDockWidget> g_dock;

static void update_visibility()
{
    if (g_dock)
        g_dock->setVisible(obs_frontend_preview_program_mode_active());
}

static void on_frontend_event(enum obs_frontend_event event, void *)
{
    if (event == OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED ||
        event == OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED)
        update_visibility();
}

bool obs_module_load(void)
{
    auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    if (!window)
        return false;

    auto *dock = new QDockWidget(QStringLiteral("Edit Panel"), window);
    dock->setObjectName(QStringLiteral("obs-edit-panel-dock"));
    dock->setAllowedAreas(Qt::AllDockWidgetAreas);

    auto *body = new QWidget(dock);
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(6, 6, 6, 6);
    auto *edit = new QLineEdit(body);
    edit->setObjectName(QStringLiteral("obs-edit-panel-input"));
    edit->setPlaceholderText(QStringLiteral("Unesi tekst..."));
    layout->addWidget(edit);
    dock->setWidget(body);

    if (!obs_frontend_add_custom_qdock("obs-edit-panel", dock)) {
        dock->deleteLater();
        return false;
    }

    g_dock = dock;
    obs_frontend_add_event_callback(on_frontend_event, nullptr);
    update_visibility();
    blog(LOG_INFO, "[obs-edit-panel] Loaded");
    return true;
}

void obs_module_unload(void)
{
    obs_frontend_remove_event_callback(on_frontend_event, nullptr);
    obs_frontend_remove_dock("obs-edit-panel");
    g_dock = nullptr;
    blog(LOG_INFO, "[obs-edit-panel] Unloaded");
}
