#pragma once
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QRegularExpression>
#include <QSlider>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QVector>

namespace counter {
struct Result {
    QString number;
    QString status = QStringLiteral("Broj nije dostupan.");
};

inline QJsonObject extract(const QString &html, const QString &name)
{
    const QRegularExpression re(QStringLiteral("(?:var\\s+)?%1\\s*=\\s*|window\\s*\\[\\s*[\"']%1[\"']\\s*\\]\\s*=\\s*").arg(name));
    auto matches = re.globalMatch(html);
    while (matches.hasNext()) {
        int start = int(matches.next().capturedEnd());
        while (start < html.size() && html[start].isSpace()) ++start;
        if (start >= html.size() || html[start] != QChar('{')) continue;
        bool quoted = false, escaped = false;
        int depth = 0;
        for (int i = start; i < html.size(); ++i) {
            const QChar c = html[i];
            if (quoted) {
                if (escaped) escaped = false;
                else if (c == QChar('\\')) escaped = true;
                else if (c == QChar('"')) quoted = false;
                continue;
            }
            if (c == QChar('"')) quoted = true;
            else if (c == QChar('{') || c == QChar('[')) ++depth;
            else if (c == QChar('}') || c == QChar(']')) {
                if (--depth != 0) continue;
                const auto doc = QJsonDocument::fromJson(html.mid(start, i - start + 1).toUtf8());
                if (doc.isObject()) return doc.object();
                break;
            }
        }
    }
    return {};
}

inline QVector<QJsonObject> find(const QJsonValue &root, const QString &key)
{
    QVector<QJsonValue> pending{root};
    QVector<QJsonObject> found;
    while (!pending.isEmpty()) {
        const auto value = pending.takeLast();
        if (value.isObject()) {
            const auto object = value.toObject();
            if (object.value(key).isObject()) found.append(object.value(key).toObject());
            for (auto it = object.begin(); it != object.end(); ++it) {
                if (it.value().isObject() || it.value().isArray()) pending.append(it.value());
            }
        } else if (value.isArray()) {
            for (const auto &item : value.toArray()) pending.append(item);
        }
    }
    return found;
}

inline QString text(const QJsonObject &object)
{
    if (object.contains(QStringLiteral("simpleText"))) return object.value(QStringLiteral("simpleText")).toString();
    QString result;
    for (const auto &part : object.value(QStringLiteral("runs")).toArray()) result += part.toObject().value(QStringLiteral("text")).toString();
    return result;
}

inline QString count(const QJsonObject &renderer)
{
    const QString original = renderer.value(QStringLiteral("originalViewCount")).toString();
    static const QRegularExpression digits(QStringLiteral("^[0-9]+$"));
    if (digits.match(original).hasMatch()) return original;
    static const QRegularExpression leading(QStringLiteral("^([0-9]+(?:[,.\\s\\x{00a0}\\x{202f}][0-9]{3})*)(?=\\s|$)"));
    const auto match = leading.match(text(renderer.value(QStringLiteral("viewCount")).toObject()).trimmed());
    if (!match.hasMatch()) return {};
    QString result = match.captured(1);
    result.remove(QRegularExpression(QStringLiteral("[^0-9]")));
    return result;
}

inline Result parse(const QString &html, const QString &expectedVideo = {}, const QString &expectedChannel = {})
{
    const auto player = extract(html, QStringLiteral("ytInitialPlayerResponse"));
    const auto initial = extract(html, QStringLiteral("ytInitialData"));
    const auto details = player.value(QStringLiteral("videoDetails")).toObject();
    const QString video = details.value(QStringLiteral("videoId")).toString();
    static const QRegularExpression id(QStringLiteral("^[A-Za-z0-9_-]{11}$"));
    if (!id.match(video).hasMatch()) return {{}, QStringLiteral("YouTube nije vratio podatke o videu.")};
    if ((!expectedVideo.isEmpty() && video != expectedVideo) ||
        (!expectedChannel.isEmpty() && details.value(QStringLiteral("channelId")).toString() != expectedChannel))
        return {{}, QStringLiteral("YouTube je vratio drugi video ili kanal.")};
    const QString playability = player.value(QStringLiteral("playabilityStatus")).toObject().value(QStringLiteral("status")).toString();
    const auto live = player.value(QStringLiteral("microformat")).toObject().value(QStringLiteral("playerMicroformatRenderer")).toObject().value(QStringLiteral("liveBroadcastDetails")).toObject();
    const bool upcoming = details.value(QStringLiteral("isUpcoming")).toBool() ||
        (playability == QStringLiteral("LIVE_STREAM_OFFLINE") && !live.contains(QStringLiteral("endTimestamp")));
    if (live.contains(QStringLiteral("endTimestamp"))) return {{}, QStringLiteral("Prenos je zavrsen.")};
    if (!upcoming && details.value(QStringLiteral("isLiveContent")).toBool() &&
        details.contains(QStringLiteral("isLive")) && !details.value(QStringLiteral("isLive")).toBool())
        return {{}, QStringLiteral("Prenos je zavrsen.")};
    if (!upcoming && playability != QStringLiteral("OK")) return {{}, QStringLiteral("YouTube ne dozvoljava citanje ovog prenosa.")};
    for (const auto &primary : find(initial.value(QStringLiteral("contents")), QStringLiteral("videoPrimaryInfoRenderer"))) {
        for (const auto &renderer : find(primary.value(QStringLiteral("viewCount")), QStringLiteral("videoViewCountRenderer"))) {
            const bool liveCount = renderer.value(QStringLiteral("isLive")).toBool();
            const bool waiting = upcoming && text(renderer.value(QStringLiteral("viewCount")).toObject()).contains(QStringLiteral("waiting"), Qt::CaseInsensitive);
            if (!liveCount && !waiting) continue;
            const QString number = count(renderer);
            if (!number.isEmpty()) return {number, upcoming ? QStringLiteral("Cekaju pocetak prenosa.") : QStringLiteral("Gledaoci uzivo.")};
        }
    }
    return {{}, upcoming ? QStringLiteral("Broj ljudi koji cekaju nije dostupan.") : QStringLiteral("Broj gledalaca nije dostupan.")};
}

struct Target { QUrl url; QString video; QString channel; };
inline Target target(QString input)
{
    input = input.trimmed();
    static const QRegularExpression channel(QStringLiteral("^UC[A-Za-z0-9_-]{22}$"));
    static const QRegularExpression video(QStringLiteral("^[A-Za-z0-9_-]{11}$"));
    static const QRegularExpression handle(QStringLiteral("^@[A-Za-z0-9._-]+$"));
    if (channel.match(input).hasMatch()) input = QStringLiteral("https://www.youtube.com/channel/") + input + QStringLiteral("/live");
    else if (video.match(input).hasMatch()) input = QStringLiteral("https://www.youtube.com/watch?v=") + input;
    else if (handle.match(input).hasMatch()) input = QStringLiteral("https://www.youtube.com/") + input + QStringLiteral("/live");
    const QUrl url(input);
    const QString host = url.host().toLower(), path = url.path();
    QString id, channelId, canonical;
    if (host == QStringLiteral("youtu.be")) id = path.section('/', 1, 1);
    else if (host == QStringLiteral("youtube.com") || host == QStringLiteral("www.youtube.com") || host == QStringLiteral("m.youtube.com")) {
        if (path == QStringLiteral("/watch")) id = QUrlQuery(url).queryItemValue(QStringLiteral("v"));
        else if (path.startsWith(QStringLiteral("/live/")) || path.startsWith(QStringLiteral("/embed/"))) id = path.section('/', 2, 2);
        else if (path.startsWith(QStringLiteral("/channel/"))) {
            channelId = path.section('/', 2, 2);
            if (!channel.match(channelId).hasMatch()) return {};
            canonical = QStringLiteral("https://www.youtube.com/channel/") + channelId + QStringLiteral("/live");
        } else if (path.startsWith(QStringLiteral("/@"))) {
            const QString name = path.section('/', 1, 1);
            if (!handle.match(name).hasMatch()) return {};
            canonical = QStringLiteral("https://www.youtube.com/") + name + QStringLiteral("/live");
        }
    }
    if (!id.isEmpty()) {
        if (!video.match(id).hasMatch()) return {};
        canonical = QStringLiteral("https://www.youtube.com/watch?v=") + id;
    }
    if (canonical.isEmpty()) return {};
    return {QUrl(canonical + (id.isEmpty() ? QStringLiteral("?hl=en") : QStringLiteral("&hl=en"))), id, channelId};
}

// OBS 31+ creates an unnamed center widget inside previewLayout. Its direct
// vertical layout contains the T-bar. Validate this structure before insertion.
inline QVBoxLayout *centerLayout(QMainWindow *main)
{
    if (!main) return nullptr;
    auto *preview = main->findChild<QHBoxLayout *>(QStringLiteral("previewLayout"));
    if (!preview || preview->count() < 3) return nullptr;
    for (int i = 1; i < preview->count() - 1; ++i) {
        auto *widget = preview->itemAt(i)->widget();
        auto *layout = widget ? qobject_cast<QVBoxLayout *>(widget->layout()) : nullptr;
        if (!layout) continue;
        for (int j = 0; j < layout->count(); ++j) {
            auto *slider = qobject_cast<QSlider *>(layout->itemAt(j)->widget());
            if (slider && slider->orientation() == Qt::Horizontal && slider->minimum() == 0 && slider->maximum() == 1023)
                return layout;
        }
    }
    return nullptr;
}

inline QLabel *addLabel(QVBoxLayout *layout)
{
    auto *parent = layout ? layout->parentWidget() : nullptr;
    if (!parent) return nullptr;
    if (auto *existing = parent->findChild<QLabel *>(QStringLiteral("youtubeCenterViewerCount"), Qt::FindDirectChildrenOnly)) return existing;
    auto *label = new QLabel(QStringLiteral("\u2014"), parent);
    label->setObjectName(QStringLiteral("youtubeCenterViewerCount"));
    label->setAlignment(Qt::AlignCenter);
    label->setContentsMargins(0, 12, 0, 6);
    label->setMinimumHeight(54);
    label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    label->setStyleSheet(QStringLiteral("color: #ffff00; background: transparent; font-size: 32px; font-weight: bold;"));
    label->setAccessibleName(QStringLiteral("Broj gledalaca"));
    // Keep OBS's leading controls and quick-transition indices intact.
    int index = layout->count();
    if (index > 0 && layout->itemAt(index - 1)->spacerItem()) --index;
    layout->insertWidget(index, label);
    return label;
}
} // namespace counter
