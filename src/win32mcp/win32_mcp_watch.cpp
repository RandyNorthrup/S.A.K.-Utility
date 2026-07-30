// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_watch.h"

#include "sak/win32mcp/win32_mcp_capture.h"

#include <QHash>
#include <QJsonDocument>
#include <QVector>

#include <algorithm>
#include <cstdlib>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace sak::win32mcp {

namespace {

// Downscale each capture to a small edge, then a fixed-size grayscale grid, so two fingerprints
// are always comparable regardless of window size and cheap to diff. A per-cell gray delta over
// kFpTolerance counts as changed; over kChangedRatio of cells changed = the view changed.
constexpr int kFpMaxEdge = 64;
constexpr int kFpGrid = 16;
constexpr int kFpTolerance = 16;
constexpr double kChangedRatio = 0.02;

// -- result + schema helpers (module-local, mirroring win32_mcp_tools) -------

ToolResult jsonResult(const QJsonObject& object) {
    return {QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)), false};
}

ToolResult errorResult(const QString& message) {
    return {QJsonDocument(QJsonObject{{QStringLiteral("error"), message}})
                .toJson(QJsonDocument::Compact),
            true};
}

QJsonObject stringProperty(const QString& description) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                       {QStringLiteral("description"), description}};
}

QJsonObject intProperty(const QString& description) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                       {QStringLiteral("description"), description}};
}

QJsonObject toolSchema(const QJsonObject& properties, const QJsonArray& required) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"), required},
                       {QStringLiteral("additionalProperties"), false}};
}

QJsonObject toolEntry(const QString& name, const QString& description, const QJsonObject& schema) {
    return QJsonObject{{QStringLiteral("name"), name},
                       {QStringLiteral("description"), description},
                       {QStringLiteral("inputSchema"), schema}};
}

qint64 clampMs(const QJsonObject& args, const QString& key, qint64 def, qint64 lo, qint64 hi) {
    const qint64 value = static_cast<qint64>(args.value(key).toDouble(static_cast<double>(def)));
    return std::min(std::max(value, lo), hi);
}

// -- fingerprints ------------------------------------------------------------

// Sample a kFpGrid x kFpGrid grayscale grid (nearest-neighbour) from a captured BGRA buffer, so
// the fingerprint is one fixed size regardless of the source resolution.
QByteArray fingerprint(const CaptureBits& bits) {
    QByteArray fp(kFpGrid * kFpGrid, 0);
    const auto* pixels = reinterpret_cast<const unsigned char*>(bits.bgra.constData());
    for (int gy = 0; gy < kFpGrid; ++gy) {
        for (int gx = 0; gx < kFpGrid; ++gx) {
            const int sx = std::min(bits.width - 1, gx * bits.width / kFpGrid);
            const int sy = std::min(bits.height - 1, gy * bits.height / kFpGrid);
            const qsizetype idx = (static_cast<qsizetype>(sy) * bits.width + sx) * 4;
            const int gray = (pixels[idx] + pixels[idx + 1] + pixels[idx + 2]) / 3;
            fp[gy * kFpGrid + gx] = static_cast<char>(gray);
        }
    }
    return fp;
}

// Fraction of grid cells whose gray value differs by more than kFpTolerance. 1.0 (fully changed)
// if the fingerprints are missing or mismatched.
double fingerprintDiff(const QByteArray& a, const QByteArray& b) {
    if (a.isEmpty() || a.size() != b.size()) {
        return 1.0;
    }
    int changed = 0;
    for (int i = 0; i < a.size(); ++i) {
        if (std::abs(static_cast<int>(static_cast<unsigned char>(a[i])) -
                     static_cast<int>(static_cast<unsigned char>(b[i]))) > kFpTolerance) {
            ++changed;
        }
    }
    return static_cast<double>(changed) / a.size();
}

// Capture the window (matched by title) and reduce it to a fingerprint + its on-screen size.
// Returns an error string on lookup/capture failure, empty on success.
QString captureFingerprint(const QString& title, QByteArray& fp, int& width, int& height) {
    WindowRect wr;
    QString err;
    if (!windowRectByTitle(title.toLower(), wr, err)) {
        return err;
    }
    CaptureBits bits;
    const CaptureRequest req{wr.hwnd, wr.left, wr.top, wr.right, wr.bottom, kFpMaxEdge};
    if (!captureBgra(req, bits, err)) {
        return err;
    }
    fp = fingerprint(bits);
    width = static_cast<int>(wr.right - wr.left);
    height = static_cast<int>(wr.bottom - wr.top);
    return {};
}

// Baselines from get_window_snapshot, keyed by lower-cased title, compared by
// compare_screenshots. The native server is single-threaded, so a plain map is safe.
QHash<QString, QByteArray> g_baselines;

// -- window existence --------------------------------------------------------

struct ExistState {
    QString needle_lower;
    bool found;
};

BOOL CALLBACK existProc(HWND hwnd, LPARAM param) {
    auto* state = reinterpret_cast<ExistState*>(param);
    if (IsWindowVisible(hwnd) == FALSE) {
        return TRUE;
    }
    const int length = GetWindowTextLengthW(hwnd);
    if (length <= 0) {
        return TRUE;
    }
    QVector<wchar_t> buffer(length + 1, L'\0');
    const int copied = GetWindowTextW(hwnd, buffer.data(), length + 1);
    if (QString::fromWCharArray(buffer.data(), copied).toLower().contains(state->needle_lower)) {
        state->found = true;
        return FALSE;
    }
    return TRUE;
}

bool windowVisibleExists(const QString& needle_lower) {
    ExistState state{needle_lower, false};
    EnumWindows(existProc, reinterpret_cast<LPARAM>(&state));
    return state.found;
}

// -- tools -------------------------------------------------------------------

QString hexColor(int r, int g, int b) {
    return QStringLiteral("#%1").arg((r << 16) | (g << 8) | b, 6, 16, QLatin1Char('0'));
}

ToolResult toolGetPixelColor(const QJsonObject& args) {
    if (!args.contains(QStringLiteral("x")) || !args.contains(QStringLiteral("y"))) {
        return errorResult(QStringLiteral("x and y are required (virtual-screen coordinates)"));
    }
    const int x = args.value(QStringLiteral("x")).toInt();
    const int y = args.value(QStringLiteral("y")).toInt();
    HDC screen = GetDC(nullptr);
    const COLORREF color = GetPixel(screen, x, y);
    ReleaseDC(nullptr, screen);
    if (color == CLR_INVALID) {
        return errorResult(QStringLiteral("Could not read that pixel (is it off-screen?)."));
    }
    const int r = GetRValue(color);
    const int g = GetGValue(color);
    const int b = GetBValue(color);
    return jsonResult(QJsonObject{{QStringLiteral("x"), x},
                                  {QStringLiteral("y"), y},
                                  {QStringLiteral("r"), r},
                                  {QStringLiteral("g"), g},
                                  {QStringLiteral("b"), b},
                                  {QStringLiteral("hex"), hexColor(r, g, b)}});
}

ToolResult toolGetWindowSnapshot(const QJsonObject& args) {
    const QString title = args.value(QStringLiteral("window_title")).toString().trimmed();
    if (title.isEmpty()) {
        return errorResult(QStringLiteral("window_title is required"));
    }
    QByteArray fp;
    int width = 0;
    int height = 0;
    const QString err = captureFingerprint(title, fp, width, height);
    if (!err.isEmpty()) {
        return errorResult(err);
    }
    g_baselines.insert(title.toLower(), fp);
    return jsonResult(QJsonObject{{QStringLiteral("window"), title},
                                  {QStringLiteral("width"), width},
                                  {QStringLiteral("height"), height},
                                  {QStringLiteral("baseline_stored"), true}});
}

ToolResult toolCompareScreenshots(const QJsonObject& args) {
    const QString title = args.value(QStringLiteral("window_title")).toString().trimmed();
    if (title.isEmpty()) {
        return errorResult(QStringLiteral("window_title is required"));
    }
    const QByteArray baseline = g_baselines.value(title.toLower());
    if (baseline.isEmpty()) {
        return errorResult(
            QStringLiteral("No baseline for that window; call get_window_snapshot first."));
    }
    QByteArray fp;
    int width = 0;
    int height = 0;
    const QString err = captureFingerprint(title, fp, width, height);
    if (!err.isEmpty()) {
        return errorResult(err);
    }
    const double ratio = fingerprintDiff(baseline, fp);
    return jsonResult(QJsonObject{{QStringLiteral("window"), title},
                                  {QStringLiteral("changed"), ratio > kChangedRatio},
                                  {QStringLiteral("diff_ratio"), ratio}});
}

ToolResult toolWaitForWindow(const QJsonObject& args) {
    const QString title = args.value(QStringLiteral("window_title")).toString().trimmed();
    if (title.isEmpty()) {
        return errorResult(QStringLiteral("window_title is required"));
    }
    const bool want_present = args.value(QStringLiteral("state")).toString().trimmed().toLower() !=
                              QLatin1String("absent");
    const qint64 timeout_ms = clampMs(args, QStringLiteral("timeout_ms"), 10'000, 200, 60'000);
    const qint64 poll_ms = clampMs(args, QStringLiteral("poll_ms"), 300, 100, 5000);
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        const bool exists = windowVisibleExists(title.toLower());
        const qint64 waited = static_cast<qint64>(GetTickCount64() - start);
        if (exists == want_present || waited >= timeout_ms) {
            return jsonResult(QJsonObject{{QStringLiteral("satisfied"), exists == want_present},
                                          {QStringLiteral("present"), exists},
                                          {QStringLiteral("waited_ms"), waited}});
        }
        Sleep(static_cast<DWORD>(poll_ms));
    }
}

ToolResult toolWaitForIdle(const QJsonObject& args) {
    const QString title = args.value(QStringLiteral("window_title")).toString().trimmed();
    if (title.isEmpty()) {
        return errorResult(QStringLiteral("window_title is required"));
    }
    const qint64 timeout_ms = clampMs(args, QStringLiteral("timeout_ms"), 10'000, 200, 60'000);
    const qint64 idle_ms = clampMs(args, QStringLiteral("idle_ms"), 600, 150, 10'000);
    const qint64 poll_ms = clampMs(args, QStringLiteral("poll_ms"), 200, 100, 2000);
    const ULONGLONG start = GetTickCount64();
    QByteArray previous;
    ULONGLONG stable_since = start;
    for (;;) {
        QByteArray fp;
        int width = 0;
        int height = 0;
        const QString err = captureFingerprint(title, fp, width, height);
        if (!err.isEmpty()) {
            return errorResult(err);
        }
        const ULONGLONG now = GetTickCount64();
        if (previous.isEmpty() || fingerprintDiff(previous, fp) > kChangedRatio) {
            previous = fp;
            stable_since = now;  // the view moved; restart the stability window
        }
        const qint64 stable = static_cast<qint64>(now - stable_since);
        const qint64 waited = static_cast<qint64>(now - start);
        if (stable >= idle_ms || waited >= timeout_ms) {
            return jsonResult(QJsonObject{{QStringLiteral("idle"), stable >= idle_ms},
                                          {QStringLiteral("waited_ms"), waited}});
        }
        Sleep(static_cast<DWORD>(poll_ms));
    }
}

// -- catalog + dispatch ------------------------------------------------------

QJsonObject windowTitleSchema() {
    return toolSchema(QJsonObject{{QStringLiteral("window_title"),
                                   stringProperty(QStringLiteral("Title substring to match."))}},
                      QJsonArray{QStringLiteral("window_title")});
}

void appendPixelTools(QJsonArray& tools) {
    tools.append(toolEntry(
        QStringLiteral("get_pixel_color"),
        QStringLiteral("Read the RGB color of one pixel at virtual-screen (x, y). Returns "
                       "r/g/b and a #rrggbb hex string."),
        toolSchema(QJsonObject{{QStringLiteral("x"), intProperty(QStringLiteral("Screen X."))},
                               {QStringLiteral("y"), intProperty(QStringLiteral("Screen Y."))}},
                   QJsonArray{QStringLiteral("x"), QStringLiteral("y")})));
    tools.append(toolEntry(
        QStringLiteral("get_window_snapshot"),
        QStringLiteral("Record a lightweight visual fingerprint of a window as the baseline for "
                       "compare_screenshots (use it to detect whether the window changed later)."),
        windowTitleSchema()));
    tools.append(toolEntry(QStringLiteral("compare_screenshots"),
                           QStringLiteral(
                               "Re-capture a window and compare it to the last get_window_snapshot "
                               "baseline. Returns changed=true/false and a 0..1 diff_ratio."),
                           windowTitleSchema()));
}

void appendWaitTools(QJsonArray& tools) {
    tools.append(toolEntry(
        QStringLiteral("wait_for_window"),
        QStringLiteral("Poll until a visible window matching the title appears (default) or is "
                       "gone (state=absent), or the timeout elapses. Returns satisfied + present "
                       "+ waited_ms."),
        toolSchema(
            QJsonObject{{QStringLiteral("window_title"),
                         stringProperty(QStringLiteral("Title substring to match."))},
                        {QStringLiteral("state"),
                         stringProperty(QStringLiteral("'present' (default) or 'absent'."))},
                        {QStringLiteral("timeout_ms"),
                         intProperty(QStringLiteral("Max wait (default 10000, max 60000)."))}},
            QJsonArray{QStringLiteral("window_title")})));
    tools.append(toolEntry(
        QStringLiteral("wait_for_idle"),
        QStringLiteral("Poll a window's visual fingerprint until it stops changing for idle_ms "
                       "(the UI has settled) or the timeout elapses. Returns idle + waited_ms."),
        toolSchema(
            QJsonObject{
                {QStringLiteral("window_title"),
                 stringProperty(QStringLiteral("Title substring to match."))},
                {QStringLiteral("idle_ms"),
                 intProperty(QStringLiteral("Quiet time required (default 600, min 150)."))},
                {QStringLiteral("timeout_ms"),
                 intProperty(QStringLiteral("Max wait (default 10000, max 60000)."))}},
            QJsonArray{QStringLiteral("window_title")})));
}

struct WatchHandler {
    QLatin1String name;
    ToolResult (*fn)(const QJsonObject&);
};

const WatchHandler kWatchHandlers[] = {
    {QLatin1String("get_pixel_color"), toolGetPixelColor},
    {QLatin1String("get_window_snapshot"), toolGetWindowSnapshot},
    {QLatin1String("compare_screenshots"), toolCompareScreenshots},
    {QLatin1String("wait_for_window"), toolWaitForWindow},
    {QLatin1String("wait_for_idle"), toolWaitForIdle},
};

}  // namespace

QJsonArray watchToolCatalog() {
    QJsonArray tools;
    appendPixelTools(tools);
    appendWaitTools(tools);
    return tools;
}

bool watchHandles(const QString& name) {
    for (const auto& entry : kWatchHandlers) {
        if (name == entry.name) {
            return true;
        }
    }
    return false;
}

ToolResult invokeWatchTool(const QString& name, const QJsonObject& args) {
    for (const auto& entry : kWatchHandlers) {
        if (name == entry.name) {
            return entry.fn(args);
        }
    }
    return errorResult(QStringLiteral("Unknown watch tool: %1").arg(name));
}

}  // namespace sak::win32mcp
