#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QBoxLayout>
#include <QFontInfo>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-edit-panel", "en-US")

static QPointer<QPlainTextEdit> g_text;
static QPointer<QTimer> g_retry_timer;

static QWidget *find_program_options(QWidget *root)
{
    const auto buttons = root->findChildren<QPushButton *>();
    for (QPushButton *button : buttons) {
        if (button->property("class").toString() != QStringLiteral("icon-dots-vert"))
            continue;

        // The transition button may be nested several levels below the
        // Studio Mode program-options container. Walk all ancestors.
        for (QWidget *candidate = button->parentWidget(); candidate;
             candidate = candidate->parentWidget()) {
            auto *layout = qobject_cast<QVBoxLayout *>(candidate->layout());
            if (layout && candidate->findChild<QSlider *>())
                return candidate;
        }
    }
    return nullptr;
}

static bool install_text_box()
{
    auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    if (!window || !obs_frontend_preview_program_mode_active())
        return false;

    QWidget *options = find_program_options(window);
    if (!options)
        return false;

    auto *layout = qobject_cast<QVBoxLayout *>(options->layout());
    auto *tbar = options->findChild<QSlider *>();
    if (!layout || !tbar)
        return false;

    if (auto *existing = options->findChild<QPlainTextEdit *>(
            QStringLiteral("obs-edit-panel-input"))) {
        g_text = existing;
        return true;
    }

    int buttonHeight = 0;
    const auto buttons = options->findChildren<QPushButton *>();
    for (QPushButton *button : buttons)
        buttonHeight = qMax(buttonHeight, button->sizeHint().height());

    auto *textBox = new QPlainTextEdit(options);
    textBox->setObjectName(QStringLiteral("obs-edit-panel-input"));
    textBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    textBox->setMinimumWidth(72);
    textBox->setFixedHeight(110 + qMax(1, buttonHeight));
    textBox->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    textBox->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    textBox->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    // Display-only text: no editing, selection, caret, or mouse focus.
    // Start empty on every OBS launch / panel creation.
    textBox->clear();
    textBox->setReadOnly(true);
    textBox->setTextInteractionFlags(Qt::NoTextInteraction);
    textBox->setFocusPolicy(Qt::NoFocus);
    textBox->setCursor(Qt::ArrowCursor);

    QFont font = textBox->font();
    int basePixelSize = QFontInfo(font).pixelSize();
    if (basePixelSize <= 0)
        basePixelSize = font.pointSizeF() > 0 ? qRound(font.pointSizeF() * 96.0 / 72.0) : 13;
    font.setPixelSize(basePixelSize + 2);
    textBox->setFont(font);
    textBox->setStyleSheet(QStringLiteral(
        "QPlainTextEdit#obs-edit-panel-input, "
        "QPlainTextEdit#obs-edit-panel-input:hover, "
        "QPlainTextEdit#obs-edit-panel-input:focus { "
        "color: #ffff00; border: none; outline: none; }"));

    const int tbarIndex = layout->indexOf(tbar);
    if (tbarIndex < 0) {
        textBox->deleteLater();
        return false;
    }

    // Keep a one-transition-button-height gap below the slider.
    layout->insertSpacing(tbarIndex + 1, qMax(1, buttonHeight));
    layout->insertWidget(tbarIndex + 2, textBox);

    auto *ackButton = new QPushButton(QStringLiteral("Pročitao sam"), options);
    ackButton->setObjectName(QStringLiteral("obs-edit-panel-ack"));
    ackButton->setCursor(Qt::ArrowCursor);
    ackButton->setFocusPolicy(Qt::NoFocus);
    ackButton->setStyleSheet(QStringLiteral(
        "QPushButton#obs-edit-panel-ack { color: red; "
        "border: 1px solid palette(mid); background: palette(button); "
        "font-weight: bold; }"));
    layout->insertWidget(tbarIndex + 3, ackButton);

    auto *blinkTimer = new QTimer(ackButton);
    blinkTimer->setInterval(500);
    auto *blinkState = new bool(true);
    QObject::connect(blinkTimer, &QTimer::timeout, ackButton, [ackButton, blinkState] {
        *blinkState = !*blinkState;
        ackButton->setStyleSheet(QStringLiteral(
            "QPushButton#obs-edit-panel-ack { color: %1; "
            "border: 1px solid palette(mid); background: palette(button); "
            "font-weight: bold; }")
            .arg(*blinkState ? QStringLiteral("red") : QStringLiteral("palette(button)")));
    });
    QObject::connect(ackButton, &QPushButton::clicked, ackButton, [ackButton, blinkTimer, blinkState] {
        blinkTimer->stop();
        delete blinkState;
        ackButton->hide();
    });
    blinkTimer->start();

    g_text = textBox;
    blog(LOG_INFO, "[obs-edit-panel] Read-only multiline text box installed below transition slider");
    return true;
}

static void on_frontend_event(enum obs_frontend_event event, void *)
{
    if (event == OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED) {
        // OBS may finish creating Studio Mode controls after this event.
        // Retry until the target container exists.
        if (g_retry_timer)
            g_retry_timer->start();
    } else if (event == OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED) {
        if (g_retry_timer)
            g_retry_timer->stop();
        g_text = nullptr; // OBS destroys the Studio Mode controls with their parent.
    }
}

bool obs_module_load(void)
{
    auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    if (!window)
        return false;

    g_retry_timer = new QTimer(window);
    g_retry_timer->setInterval(250);
    QObject::connect(g_retry_timer, &QTimer::timeout, window, [] {
        if (install_text_box() && g_retry_timer)
            g_retry_timer->stop();
    });

    obs_frontend_add_event_callback(on_frontend_event, nullptr);
    if (obs_frontend_preview_program_mode_active())
        g_retry_timer->start();

    blog(LOG_INFO, "[obs-edit-panel] Loaded");
    return true;
}

void obs_module_unload(void)
{
    obs_frontend_remove_event_callback(on_frontend_event, nullptr);
    if (g_retry_timer)
        g_retry_timer->stop();
    g_retry_timer = nullptr;
    g_text = nullptr;
    blog(LOG_INFO, "[obs-edit-panel] Unloaded");
}
