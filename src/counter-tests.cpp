#include "counter-core.hpp"
#include "winhttp-client.hpp"
#include <QApplication>
#include <QPointer>
#include <QHelpEvent>

static void check(bool value, const char *message)
{
    if (!value) qFatal("Counter test failed: %s", message);
}

static QString sample(QString number, bool upcoming = false, bool ended = false, bool liveCount = true)
{
    QJsonObject details{{"videoId", "abcdefghijk"}, {"channelId", "UCQXPKDYp4U9JkTWAON_AhOw"},
        {"isLiveContent", true}, {"isLive", !upcoming && !ended}, {"isUpcoming", upcoming}};
    QJsonObject broadcast{{"isLiveNow", !upcoming && !ended}};
    if (ended) broadcast.insert("endTimestamp", "2026-10-23T00:00:00Z");
    QJsonObject player{{"videoDetails", details},
        {"playabilityStatus", QJsonObject{{"status", upcoming ? "LIVE_STREAM_OFFLINE" : "OK"}}},
        {"microformat", QJsonObject{{"playerMicroformatRenderer", QJsonObject{{"liveBroadcastDetails", broadcast}}}}}};
    QJsonObject renderer{{"originalViewCount", number}, {"isLive", liveCount},
        {"viewCount", QJsonObject{{"simpleText", number + (upcoming ? " waiting" : " watching now")}}}};
    QJsonObject initial{{"contents", QJsonObject{{"videoPrimaryInfoRenderer", QJsonObject{{"viewCount", QJsonObject{{"videoViewCountRenderer", renderer}}}}}}}};
    return "var ytInitialPlayerResponse = " + QString::fromUtf8(QJsonDocument(player).toJson(QJsonDocument::Compact)) +
        ";var ytInitialData = " + QString::fromUtf8(QJsonDocument(initial).toJson(QJsonDocument::Compact)) + ";";
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    check(counter::parse(sample("217")).number == "217", "live count");
    check(counter::parse(sample("0")).number == "0", "live zero");
    check(counter::parse(sample("12", true)).number == "12", "waiting count");
    check(counter::parse(sample("0", true)).number == "0", "waiting zero");
    check(counter::parse(sample("3", true, false, false)).number == "3", "waiting text without live flag");
    check(counter::parse(sample("3", false, true)).number.isEmpty(), "ended ignores view count");
    check(counter::parse(sample("3", false, false, false)).number.isEmpty(), "total views not used");
    check(counter::parse("Sign in to confirm").number.isEmpty(), "missing data");
    check(counter::parse(sample("3"), "12345678901").number.isEmpty(), "video mismatch");
    check(counter::parse(sample("3"), {}, "UCwrong").number.isEmpty(), "channel mismatch");
    check(counter::count(QJsonObject{{"viewCount", QJsonObject{{"simpleText", "1.2K watching now"}}}}).isEmpty(), "no guessed precision");
    check(counter::count(QJsonObject{{"viewCount", QJsonObject{{"simpleText", "1,234 watching now"}}}}) == "1234", "thousands separators");
    check(counter::extract("window[\"ytInitialData\"]={\"a\":\"} escaped \\\" \\\\ end\"};", "ytInitialData").contains("a"), "quoted braces and escape sequences");
    check(counter::target("@TheLodgeLive").url.host() == "www.youtube.com", "channel handle");
    check(counter::target("https://www.youtube.com/@BeogradCom/live").url.path() == "/@BeogradCom/live", "channel url");
    check(counter::target("UCQXPKDYp4U9JkTWAON_AhOw").channel == "UCQXPKDYp4U9JkTWAON_AhOw", "channel id");
    check(counter::target("https://youtu.be/abcdefghijk").video == "abcdefghijk", "short url");
    check(counter::target("https://example.com/watch?v=abcdefghijk").url.isEmpty(), "non youtube rejected");
    check(counter::target("https://youtube.com/@test/live?other=1").url.query() == "hl=en", "only canonical requests");

    QMainWindow main;
    check(!counter::statusLayout(&main), "no unrecognized status bar injection");
    auto *bar = new QStatusBar(&main);
    main.setStatusBar(bar);
    auto *status = new QWidget(bar);
    status->setObjectName("StatusBarWidget");
    auto *row = new QHBoxLayout(status);
    row->setContentsMargins(0, 0, 0, 0);
    auto *frame = new QWidget(status);
    frame->setObjectName("messageFrame");
    auto *messages = new QHBoxLayout(frame);
    messages->setContentsMargins(0, 0, 0, 0);
    auto *message = new QLabel(frame);
    message->setObjectName("message");
    QFont normalFont = message->font();
    normalFont.setPointSize(9);
    normalFont.setWeight(QFont::Normal);
    message->setFont(normalFont);
    messages->addWidget(message, 1);
    row->addWidget(frame, 1);
    row->addWidget(new QLabel("CPU 1.0%", status));
    bar->addPermanentWidget(status, 1);
    check(counter::statusLayout(&main) == messages, "existing left message frame");
    QPointer<QLabel> label = counter::addStatusLabel(messages);
    check(label && messages->indexOf(label) == 0, "counter is first item on left");
    check(counter::addStatusLabel(messages) == label && messages->count() == 2, "no duplicate labels");
    check(label->alignment().testFlag(Qt::AlignLeft), "left aligned");
    check(label->font() == message->font(), "same font as native status text");
    check(label->styleSheet().isEmpty(), "no custom color or large bold font");
    check(label->toolTip().isEmpty() && label->statusTip().isEmpty() && label->whatsThis().isEmpty(), "no hover hint properties");
    QHelpEvent help(QEvent::ToolTip, QPoint(1, 1), QPoint(1, 1));
    check(QApplication::sendEvent(label, &help) && help.isAccepted(), "tooltip event consumed");
    normalFont.setPointSize(11);
    message->setFont(normalFont);
    counter::syncStatusFont(label);
    check(label->font() == message->font(), "follows status font change");
    label->setText("217");
    message->setText("Recording started");
    main.resize(900, 600);
    main.show();
    app.processEvents();
    check(label->text() == "217" && label->isVisible(), "number remains during status messages");
    check(label->x() < message->x(), "number stays left of existing message");
    main.resize(700, 400);
    app.processEvents();
    check(label->isVisible(), "counter survives resizing");
    delete label.data();
    check(label.isNull() && messages->count() == 1, "clean plugin unload");
    label = counter::addStatusLabel(messages);
    check(label && messages->indexOf(label) == 0, "counter reattachment");
    qInfo("All parser, source validation and status bar UI tests passed.");
    std::atomic_bool cancelled{true};
    check(!native_http::fetch(QUrl("https://example.com/"), cancelled).error.isEmpty(), "cancelled request");
    cancelled.store(false);
    check(!native_http::fetch(QUrl("http://example.com/"), cancelled).error.isEmpty(), "HTTPS required");
    const QString smokeUrl = qEnvironmentVariable("COUNTER_HTTPS_SMOKE_URL");
    if (!smokeUrl.isEmpty()) {
        const auto response = native_http::fetch(QUrl(smokeUrl), cancelled);
        if (!response.error.isEmpty()) qFatal("Windows HTTPS smoke test failed: %s", qPrintable(response.error));
        check(!response.body.isEmpty(), "real Windows HTTPS response");
        qInfo("Windows HTTPS smoke test passed without Qt Network/TLS.");
    }
    return 0;
}
