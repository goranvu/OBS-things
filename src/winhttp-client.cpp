#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include "winhttp-client.hpp"
#include <chrono>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace native_http {
namespace {
constexpr int MaxBytes = 8 * 1024 * 1024;
using Clock = std::chrono::steady_clock;

struct Handle {
    HINTERNET value = nullptr;
    explicit Handle(HINTERNET h = nullptr) : value(h) {}
    ~Handle() { if (value) WinHttpCloseHandle(value); }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
};

struct Completion {
    std::mutex mutex;
    std::condition_variable ready;
    DWORD status = 0;
    DWORD error = 0;
    DWORD bytes = 0;
    bool closed = false;

    void reset()
    {
        std::lock_guard<std::mutex> guard(mutex);
        status = 0; error = 0; bytes = 0;
    }

    DWORD wait(DWORD expected, Clock::time_point deadline, const std::atomic_bool &cancelled)
    {
        std::unique_lock<std::mutex> guard(mutex);
        while (!error && status != expected && !cancelled.load()) {
            const auto now = Clock::now();
            if (now >= deadline) return ERROR_WINHTTP_TIMEOUT;
            ready.wait_until(guard, (std::min)(deadline, now + std::chrono::milliseconds(100)));
        }
        if (cancelled.load()) return ERROR_WINHTTP_OPERATION_CANCELLED;
        return error;
    }
};

void CALLBACK callback(HINTERNET, DWORD_PTR context, DWORD status, LPVOID data, DWORD length)
{
    if (!context) return;
    auto &completion = *reinterpret_cast<Completion *>(context);
    std::lock_guard<std::mutex> guard(completion.mutex);
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) completion.closed = true;
    else if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
        completion.error = data ? static_cast<WINHTTP_ASYNC_RESULT *>(data)->dwError : ERROR_WINHTTP_INTERNAL_ERROR;
    } else if (status == WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE ||
               status == WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE ||
               status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) {
        completion.status = status;
        completion.bytes = length;
    }
    completion.ready.notify_all();
}

QString errorText(DWORD code)
{
    wchar_t *buffer = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    const QString message = length && buffer ? QString::fromWCharArray(buffer, int(length)).trimmed() : QStringLiteral("Mrezna greska");
    if (buffer) LocalFree(buffer);
    return QStringLiteral("Windows HTTPS (%1): %2").arg(code).arg(message);
}

struct Request {
    HINTERNET value = nullptr;
    Completion &completion;
    bool callbackInstalled = false;
    explicit Request(HINTERNET h, Completion &c) : value(h), completion(c) {}
    ~Request()
    {
        if (!value) return;
        // All calls on this async request are made on this worker. Closing after
        // the last API call returns cancels outstanding I/O without a UI block.
        WinHttpCloseHandle(value);
        if (callbackInstalled) {
            std::unique_lock<std::mutex> guard(completion.mutex);
            // HANDLE_CLOSING is the final callback. Keep its context and read
            // buffer alive until it arrives, including on cancellation/unload.
            completion.ready.wait(guard, [this]() { return completion.closed; });
        }
    }
};
} // namespace

Reply fetch(const QUrl &url, const std::atomic_bool &cancelled)
{
    if (cancelled.load()) return {{}, errorText(ERROR_WINHTTP_OPERATION_CANCELLED)};
    if (url.scheme() != QStringLiteral("https") || url.host().isEmpty())
        return {{}, QStringLiteral("Potrebna je ispravna HTTPS adresa.")};
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    Handle session(WinHttpOpen(L"Mozilla/5.0 OBSViewerCounter/2.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
    if (!session.value) return {{}, errorText(GetLastError())};
    if (!WinHttpSetTimeouts(session.value, 2000, 3000, 3000, 8000)) return {{}, errorText(GetLastError())};
    const auto host = url.host().toStdWString();
    Handle connection(WinHttpConnect(session.value, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection.value) return {{}, errorText(GetLastError())};

    Completion completion;
    // Buffer must outlive Request, whose destructor waits for the final callback.
    char buffer[32768];
    QString path = url.path(QUrl::FullyEncoded);
    if (path.isEmpty()) path = QStringLiteral("/");
    if (url.hasQuery()) path += QStringLiteral("?") + url.query(QUrl::FullyEncoded);
    const auto widePath = path.toStdWString();
    Request request(WinHttpOpenRequest(connection.value, L"GET", widePath.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE | WINHTTP_FLAG_REFRESH), completion);
    if (!request.value) return {{}, errorText(GetLastError())};
    DWORD_PTR context = reinterpret_cast<DWORD_PTR>(&completion);
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)))
        return {{}, errorText(GetLastError())};
    if (WinHttpSetStatusCallback(request.value, callback,
        WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0) == WINHTTP_INVALID_STATUS_CALLBACK)
        return {{}, errorText(GetLastError())};
    request.callbackInstalled = true;

    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect)))
        return {{}, errorText(GetLastError())};
    DWORD auth = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_AUTOLOGON_POLICY, &auth, sizeof(auth)))
        return {{}, errorText(GetLastError())};
    // Certificate and hostname verification stay enabled using Windows Schannel.
    completion.reset();
    const wchar_t *headers = L"Accept-Language: en-US,en;q=0.9\r\nCache-Control: no-cache\r\nPragma: no-cache\r\n";
    if (!WinHttpSendRequest(request.value, headers, DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, context))
        return {{}, errorText(GetLastError())};
    DWORD error = completion.wait(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, deadline, cancelled);
    if (error) return {{}, errorText(error)};
    completion.reset();
    if (!WinHttpReceiveResponse(request.value, nullptr)) return {{}, errorText(GetLastError())};
    error = completion.wait(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, deadline, cancelled);
    if (error) return {{}, errorText(error)};
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) return {{}, errorText(GetLastError())};
    if (status != 200) return {{}, QStringLiteral("YouTube HTTP %1").arg(status)};

    DWORD urlBytes = 0;
    WinHttpQueryOption(request.value, WINHTTP_OPTION_URL, nullptr, &urlBytes);
    if (urlBytes) {
        std::vector<wchar_t> finalUrl(size_t(urlBytes / sizeof(wchar_t)) + 1, 0);
        if (WinHttpQueryOption(request.value, WINHTTP_OPTION_URL, finalUrl.data(), &urlBytes)) {
            const QUrl finalAddress(QString::fromWCharArray(finalUrl.data()));
            if (finalAddress.scheme() != QStringLiteral("https")) return {{}, QStringLiteral("Odbijeno preusmeravanje van HTTPS-a.")};
            const QString originalHost = url.host().toLower();
            const QString finalHost = finalAddress.host().toLower();
            if (originalHost.endsWith(QStringLiteral("youtube.com")) && finalHost != QStringLiteral("www.youtube.com") &&
                finalHost != QStringLiteral("youtube.com") && finalHost != QStringLiteral("m.youtube.com"))
                return {{}, QStringLiteral("YouTube trazi saglasnost ili prijavu.")};
        }
    }

    QByteArray body;
    while (true) {
        if (Clock::now() >= deadline) return {{}, errorText(ERROR_WINHTTP_TIMEOUT)};
        completion.reset();
        if (!WinHttpReadData(request.value, buffer, DWORD(sizeof(buffer)), nullptr)) return {{}, errorText(GetLastError())};
        error = completion.wait(WINHTTP_CALLBACK_STATUS_READ_COMPLETE, deadline, cancelled);
        if (error) return {{}, errorText(error)};
        const DWORD bytes = completion.bytes;
        if (!bytes) break;
        if (body.size() + bytes > MaxBytes) return {{}, QStringLiteral("YouTube odgovor je prevelik.")};
        body.append(buffer, int(bytes));
    }
    return {body, {}};
}
} // namespace native_http
