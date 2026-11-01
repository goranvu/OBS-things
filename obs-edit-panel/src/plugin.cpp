#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QBoxLayout>
#include <QLineEdit>
#include <QMainWindow>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-edit-panel", "en-US")

static QPointer<QLineEdit> g_edit;

static QWidget *find_program_options(QWidget *root)
{
    const auto buttons = root->findChildren<QPushButton *>();
    for (QPushButton *button : buttons) {
        if (button->property("class").toString() != QStringLiteral("icon-dots-vert"))
            continue;

        QWidget *candidate = button->parentWidget();
        if (!candidate)
            continue;

        auto *layout = qobject_cast<QVBoxLayout *>(candidate->layout());
        if (layout && candidate->findChild<QSlider *>())
            return candidate;
    }
    return nullptr;
}

static void install_edit_field()
{
    auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    if (!window || !obs_frontend_preview_program_mode_active())
        return;

    QWidget *options = find_program_options(window);
    if (!options)
        return;

    auto *layout = qobject_cast<QVBoxLayout *>(options->layout());
    auto *tbar = options->findChild<QSlider *>();
    if (!layout || !tbar)
        return;

    if (options->findChild<QLineEdit *>(QStringLiteral("obs-edit-panel-input")))
        return;

    int buttonHeight = 0;
    const auto buttons = options->findChildren<QPushButton *>();
    for (QPushButton *button : buttons)
        buttonHeight = qMax(buttonHeight, button->sizeHint().height());

    auto *edit = new QLineEdit(options);
    edit->setObjectName(QStringLiteral("obs-edit-panel-input"));
    edit->setPlaceholderText(QStringLiteral("Unesi tekst..."));
    edit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    edit->setMinimumWidth(72);

    const int tbarIndex = layout->indexOf(tbar);
    if (tbarIndex < 0) {
        edit->deleteLater();
        return;
    }

    // Leave a vertical gap equal to two transition-button heights.
    layout->insertSpacing(tbarIndex, qMax(1, buttonHeight * 2));
    layout->insertWidget(tbarIndex + 1, edit);
    g_edit = edit;
    blog(LOG_INFO, "[obs-edit-panel] Inline text field installed in Studio Mode");
}

static void on_frontend_event(enum obs_frontend_event event, void *)
{
    if (event == OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED) {
        // OBS creates the Preview/Program controls during this event.
        QTimer::singleShot(0, install_edit_field);
    } else if (event == OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED) {
        g_edit = nullptr; // OBS destroys the Studio Mode controls with their parent.
    }
}

bool obs_module_load(void)
{
    if (!obs_frontend_get_main_window())
        return false;

    obs_frontend_add_event_callback(on_frontend_event, nullptr);
    if (obs_frontend_preview_program_mode_active())
        QTimer::singleShot(0, install_edit_field);

    blog(LOG_INFO, "[obs-edit-panel] Loaded");
    return true;
}

void obs_module_unload(void)
{
    obs_frontend_remove_event_callback(on_frontend_event, nullptr);
    g_edit = nullptr;
    blog(LOG_INFO, "[obs-edit-panel] Unloaded");
}
