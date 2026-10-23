#pragma once
#include <QByteArray>
#include <QString>
#include <QUrl>
#include <atomic>

namespace native_http {
struct Reply { QByteArray body; QString error; };
Reply fetch(const QUrl &url, const std::atomic_bool &cancelled);
}
