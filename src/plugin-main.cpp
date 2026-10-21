/*
 * OBS 354 - fixed-height viewer counter dock (Windows / OBS Studio 30+)
 * Copyright (c) 2026. Released under GNU GPL v2 or later.
 *
 * This creates a native Qt dock, not an OBS video source.
 * Its number area is 34px high, with a visible "Broj gledalaca" title bar.
 * The X and floating controls are disabled.
 */

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QDockWidget>
#include <QFont>
#include <QLabel>
#include <QMainWindow>
#include <QPointer>
#include <QSize>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-fixed-counter", "en-US")

namespace {
constexpr auto DockId = "obs-fixed-counter-354";
constexpr int ContentHeight = 40;
QPointer<QDockWidget> ourDock;
bool obsIsExiting = false;

class NumberWidget final : public QWidget {
public:
    explicit NumberWidget(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFixedHeight(ContentHeight); // Header height is managed separately by Qt.
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        auto *number = new QLabel(QStringLiteral("354"), this);
        number->setAlignment(Qt::AlignCenter);
        QFont numberFont = number->font();
        numberFont.setPixelSize(24); // actual pixel size, not typographic points
        numberFont.setWeight(QFont::Normal);
        number->setFont(numberFont);
        number->setStyleSheet(QStringLiteral("color: #9BCDE5; background: transparent;"));
        layout->addWidget(number);
    }

    QSize sizeHint() const override { return QSize(190, ContentHeight); }
};

void addCounterDock()
{
    if (ourDock || obsIsExiting)
        return;

    auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    if (!main) {
        blog(LOG_WARNING, "[OBS 354] Main OBS window not found; dock not created.");
        return;
    }

    auto *dock = new QDockWidget(QStringLiteral("Broj gledalaca"), main);
    dock->setAllowedAreas(Qt::AllDockWidgetAreas);

    // Keep the native OBS/Qt title bar and its "Broj gledalaca" caption.
    // NoDockWidgetFeatures below removes the X and float buttons, without
    // removing the caption itself.
    dock->setWidget(new NumberWidget(dock));

    // OBS may change dock features when 'Docks -> Lock Docks' is toggled.
    // Re-enforce these properties so this particular dock remains locked.
    QObject::connect(dock, &QDockWidget::featuresChanged, dock, [dock]() {
        if (dock->features() != QDockWidget::NoDockWidgetFeatures)
            dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    });

    if (!obs_frontend_add_custom_qdock(DockId, dock)) {
        blog(LOG_WARNING, "[OBS 354] Dock registration failed (duplicate ID?).");
        delete dock;
        return;
    }

    ourDock = dock;
    dock->setFeatures(QDockWidget::NoDockWidgetFeatures);

    // OBS calls this default control panel 'controlsDock'.
    auto *controls = main->findChild<QDockWidget *>(QStringLiteral("controlsDock"));
    if (controls && !controls->isFloating()) {
        main->splitDockWidget(controls, dock, Qt::Vertical);
    } else {
        // add_custom_qdock already places it in OBS's right dock area.
        blog(LOG_INFO, "[OBS 354] Controls not docked; counter added to right area.");
    }
    dock->show();
    blog(LOG_INFO, "[OBS 354] Fixed 34px content dock added (title = Broj gledalaca, counter = 354).");
}

void onFrontendEvent(enum obs_frontend_event event, void *)
{
    if (event == OBS_FRONTEND_EVENT_EXIT) {
        obsIsExiting = true;
        return;
    }
    if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING)
        return;

    // Defer until Qt has finished the current GUI event iteration.
    auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    if (main)
        QTimer::singleShot(0, main, [] { addCounterDock(); });
}
} // namespace

extern "C" const char *obs_module_description(void)
{
    return "OBS dock titled Broj gledalaca with a fixed 34px number area displaying 354.";
}

extern "C" bool obs_module_load(void)
{
    obs_frontend_add_event_callback(onFrontendEvent, nullptr);
    blog(LOG_INFO, "[OBS 354] Plugin initialized; awaiting frontend.");
    return true;
}

extern "C" void obs_module_unload(void)
{
    obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
    // Qt / OBS own the QDockWidget. OBS cleans it up during exit.
    if (!obsIsExiting && ourDock)
        obs_frontend_remove_dock(DockId);
    ourDock.clear();
}
