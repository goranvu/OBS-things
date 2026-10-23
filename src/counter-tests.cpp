#include "counter-core.hpp"
#include <QApplication>
#include <QPointer>
#include <QPushButton>

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
    auto *central = new QWidget(&main);
    main.setCentralWidget(central);
    auto *preview = new QHBoxLayout(central);
    preview->setObjectName("previewLayout");
    preview->addWidget(new QWidget(central));
    preview->addWidget(new QWidget(central));
    check(!counter::centerLayout(&main), "no injection outside studio layout");
    for (int pass = 0; pass < 3; ++pass) {
        auto *options = new QWidget(central);
        auto *layout = new QVBoxLayout(options);
        layout->addStretch();
        layout->addLayout(new QHBoxLayout());
        layout->addLayout(new QHBoxLayout());
        auto *slider = new QSlider(Qt::Horizontal, options);
        slider->setRange(0, 1023);
        layout->addWidget(slider);
        layout->addStretch();
        preview->insertWidget(1, options);
        check(counter::centerLayout(&main) == layout, "transition layout detection");
        QPointer<QLabel> label = counter::addLabel(layout);
        check(label && layout->indexOf(label) == 4, "number below tbar");
        check(counter::addLabel(layout) == label && layout->count() == 6, "no duplicate numbers");
        auto *quick = new QPushButton(options);
        quick->setProperty("id", 1);
        layout->insertWidget(3, quick);
        check(layout->indexOf(label) > layout->indexOf(slider), "new quick transitions stay before counter");
        delete options;
        check(label.isNull(), "counter removed safely with studio controls");
        check(!counter::centerLayout(&main), "studio disabled");
    }
    qInfo("All parser, source validation and Studio Mode layout tests passed.");
    return 0;
}
