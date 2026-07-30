// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_ocr.h"

#include "sak/win32mcp/win32_mcp_capture.h"

#include <QJsonDocument>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// WIN32_LEAN_AND_MEAN drops core COM headers; CoInitializeEx/CoUninitialize come from objbase.
#include <objbase.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Storage.Streams.h>

namespace sak::win32mcp {

namespace {

using winrt::Windows::Graphics::Imaging::BitmapAlphaMode;
using winrt::Windows::Graphics::Imaging::BitmapPixelFormat;
using winrt::Windows::Graphics::Imaging::SoftwareBitmap;
using winrt::Windows::Media::Ocr::OcrEngine;
using winrt::Windows::Media::Ocr::OcrResult;
using winrt::Windows::Security::Cryptography::CryptographicBuffer;

// Cap the OCR working image edge so a huge surface cannot spin the recognizer or the downscale;
// captureBgra downscales to fit and reports the scale so boxes map back to screen pixels.
constexpr int kOcrMaxEdgeCap = 4096;

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

// COM apartment scoped to one tool call (MTA, tolerating an already-STA thread). WinRT
// activation + the blocking RecognizeAsync().get() both require COM initialized on the thread.
class ComApartment {
public:
    ComApartment() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owned_ = (hr == S_OK || hr == S_FALSE);
    }
    ~ComApartment() {
        if (owned_) {
            CoUninitialize();
        }
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

private:
    bool owned_{false};
};

QString hstringToQString(const winrt::hstring& text) {
    return QString::fromWCharArray(text.c_str(), static_cast<int>(text.size()));
}

// Recognize the captured BGRA pixels. Returns an error string on WinRT failure, empty on
// success (with `out` filled).
QString recognize(const OcrEngine& engine, const CaptureBits& bits, OcrResult& out) {
    try {
        const auto* first = reinterpret_cast<const uint8_t*>(bits.bgra.constData());
        const winrt::array_view<const uint8_t> view(first, first + bits.bgra.size());
        const auto buffer = CryptographicBuffer::CreateFromByteArray(view);
        SoftwareBitmap bitmap(
            BitmapPixelFormat::Bgra8, bits.width, bits.height, BitmapAlphaMode::Ignore);
        bitmap.CopyFromBuffer(buffer);
        out = engine.RecognizeAsync(bitmap).get();
        return {};
    } catch (const winrt::hresult_error& e) {
        return hstringToQString(e.message());
    }
}

QJsonObject buildPlain(const OcrResult& result) {
    return QJsonObject{{QStringLiteral("text"), hstringToQString(result.Text())},
                       {QStringLiteral("line_count"), static_cast<int>(result.Lines().Size())}};
}

// Map a recognized word's bounding rect (in downscaled capture pixels) back to absolute
// virtual-screen coordinates: divide out the capture scale, then add the capture origin.
QJsonObject wordBox(const winrt::Windows::Foundation::Rect& rect,
                    long origin_x,
                    long origin_y,
                    double inv_scale) {
    return QJsonObject{
        {QStringLiteral("x"), static_cast<int>(std::llround(origin_x + rect.X * inv_scale))},
        {QStringLiteral("y"), static_cast<int>(std::llround(origin_y + rect.Y * inv_scale))},
        {QStringLiteral("width"), static_cast<int>(std::llround(rect.Width * inv_scale))},
        {QStringLiteral("height"), static_cast<int>(std::llround(rect.Height * inv_scale))}};
}

QJsonObject buildStructured(const OcrResult& result, long origin_x, long origin_y, double scale) {
    const double inv = (scale > 0.0) ? (1.0 / scale) : 1.0;
    QJsonArray lines;
    int word_count = 0;
    for (const auto& line : result.Lines()) {
        QJsonArray words;
        for (const auto& word : line.Words()) {
            QJsonObject entry = wordBox(word.BoundingRect(), origin_x, origin_y, inv);
            entry.insert(QStringLiteral("text"), hstringToQString(word.Text()));
            words.append(entry);
            ++word_count;
        }
        lines.append(QJsonObject{{QStringLiteral("text"), hstringToQString(line.Text())},
                                 {QStringLiteral("words"), words}});
    }
    return QJsonObject{{QStringLiteral("word_count"), word_count},
                       {QStringLiteral("line_count"), static_cast<int>(result.Lines().Size())},
                       {QStringLiteral("lines"), lines}};
}

OcrEngine makeEngine() {
    try {
        return OcrEngine::TryCreateFromUserProfileLanguages();
    } catch (const winrt::hresult_error&) {
        return nullptr;
    }
}

QString noOcrLanguageText() {
    return QStringLiteral(
        "No OCR language is installed. Add a language with text recognition in "
        "Windows Settings > Time & language > Language.");
}

// Shared flow for every OCR tool: create the engine, capture the target (downscaled to the
// engine's limit), recognize, and shape the reply. origin for box mapping is (req.left, top).
ToolResult runOcrCapture(CaptureRequest req, bool structured) {
    ComApartment com;
    const OcrEngine engine = makeEngine();
    if (!engine) {
        return errorResult(noOcrLanguageText());
    }
    req.max_edge = std::min<int>(static_cast<int>(engine.MaxImageDimension()), kOcrMaxEdgeCap);
    CaptureBits bits;
    QString err;
    if (!captureBgra(req, bits, err)) {
        return errorResult(err);
    }
    OcrResult result{nullptr};
    const QString ocr_err = recognize(engine, bits, result);
    if (!ocr_err.isEmpty()) {
        return errorResult(ocr_err);
    }
    return jsonResult(structured ? buildStructured(result, req.left, req.top, bits.scale)
                                 : buildPlain(result));
}

bool resolveRegion(const QJsonObject& args, CaptureRequest& req, QString& err) {
    for (const char* key : {"x", "y", "width", "height"}) {
        if (!args.contains(QLatin1String(key))) {
            err = QStringLiteral("x, y, width, and height are required");
            return false;
        }
    }
    const int x = args.value(QStringLiteral("x")).toInt();
    const int y = args.value(QStringLiteral("y")).toInt();
    const int w = args.value(QStringLiteral("width")).toInt();
    const int h = args.value(QStringLiteral("height")).toInt();
    if (w <= 0 || h <= 0) {
        err = QStringLiteral("width and height must be positive");
        return false;
    }
    req = CaptureRequest{nullptr, x, y, x + w, y + h, 0};
    return true;
}

CaptureRequest screenRequest() {
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return CaptureRequest{nullptr, x, y, x + w, y + h, 0};
}

ToolResult toolOcrWindow(const QJsonObject& args) {
    const QString title = args.value(QStringLiteral("window_title")).toString().trimmed();
    if (title.isEmpty()) {
        return errorResult(QStringLiteral("window_title is required"));
    }
    WindowRect wr;
    QString err;
    if (!windowRectByTitle(title.toLower(), wr, err)) {
        return errorResult(err);
    }
    return runOcrCapture(CaptureRequest{wr.hwnd, wr.left, wr.top, wr.right, wr.bottom, 0}, false);
}

ToolResult toolOcrRegion(const QJsonObject& args) {
    CaptureRequest req{};
    QString err;
    if (!resolveRegion(args, req, err)) {
        return errorResult(err);
    }
    return runOcrCapture(req, false);
}

ToolResult toolOcrRegionStructured(const QJsonObject& args) {
    CaptureRequest req{};
    QString err;
    if (!resolveRegion(args, req, err)) {
        return errorResult(err);
    }
    return runOcrCapture(req, true);
}

ToolResult toolOcrScreen(const QJsonObject&) {
    return runOcrCapture(screenRequest(), false);
}

ToolResult toolOcrScreenStructured(const QJsonObject&) {
    return runOcrCapture(screenRequest(), true);
}

// -- text search / wait (OCR-backed) -----------------------------------------

struct WordHit {
    QString text;
    int x;
    int y;
    int w;
    int h;
};

void appendWords(const OcrResult& result, long ox, long oy, double inv, QVector<WordHit>& out) {
    for (const auto& line : result.Lines()) {
        for (const auto& word : line.Words()) {
            const auto rect = word.BoundingRect();
            out.append(WordHit{hstringToQString(word.Text()),
                               static_cast<int>(std::llround(ox + rect.X * inv)),
                               static_cast<int>(std::llround(oy + rect.Y * inv)),
                               static_cast<int>(std::llround(rect.Width * inv)),
                               static_cast<int>(std::llround(rect.Height * inv))});
        }
    }
}

// OCR the target and collect every recognized word with an absolute screen box. Returns an
// error string (missing language / capture / recognize failure), empty on success.
QString ocrCollect(CaptureRequest req, QVector<WordHit>& out) {
    ComApartment com;
    const OcrEngine engine = makeEngine();
    if (!engine) {
        return noOcrLanguageText();
    }
    req.max_edge = std::min<int>(static_cast<int>(engine.MaxImageDimension()), kOcrMaxEdgeCap);
    CaptureBits bits;
    QString err;
    if (!captureBgra(req, bits, err)) {
        return err;
    }
    OcrResult result{nullptr};
    const QString ocr_err = recognize(engine, bits, result);
    if (!ocr_err.isEmpty()) {
        return ocr_err;
    }
    appendWords(result, req.left, req.top, (bits.scale > 0.0) ? (1.0 / bits.scale) : 1.0, out);
    return {};
}

// A text tool targets a window (window_title present) or the full screen (absent).
bool resolveTextTarget(const QJsonObject& args, CaptureRequest& req, QString& err) {
    const QString title = args.value(QStringLiteral("window_title")).toString().trimmed();
    if (title.isEmpty()) {
        req = screenRequest();
        return true;
    }
    WindowRect wr;
    if (!windowRectByTitle(title.toLower(), wr, err)) {
        return false;
    }
    req = CaptureRequest{wr.hwnd, wr.left, wr.top, wr.right, wr.bottom, 0};
    return true;
}

QJsonArray matchesToJson(const QVector<WordHit>& hits, const QString& query) {
    const QString needle = query.toLower();
    QJsonArray matches;
    for (const auto& hit : hits) {
        if (!hit.text.toLower().contains(needle)) {
            continue;
        }
        matches.append(QJsonObject{{QStringLiteral("text"), hit.text},
                                   {QStringLiteral("x"), hit.x},
                                   {QStringLiteral("y"), hit.y},
                                   {QStringLiteral("width"), hit.w},
                                   {QStringLiteral("height"), hit.h}});
    }
    return matches;
}

// find_text_on_screen and assert_text_visible share the OCR + filter flow; assert mode adds a
// boolean verdict, find mode echoes the query.
ToolResult textQuery(const QJsonObject& args, bool assert_mode) {
    const QString query = args.value(QStringLiteral("text")).toString().trimmed();
    if (query.isEmpty()) {
        return errorResult(QStringLiteral("text is required"));
    }
    CaptureRequest req{};
    QString err;
    if (!resolveTextTarget(args, req, err)) {
        return errorResult(err);
    }
    QVector<WordHit> hits;
    const QString ocr_err = ocrCollect(req, hits);
    if (!ocr_err.isEmpty()) {
        return errorResult(ocr_err);
    }
    const QJsonArray matches = matchesToJson(hits, query);
    QJsonObject out{{QStringLiteral("count"), matches.size()},
                    {QStringLiteral("matches"), matches}};
    if (assert_mode) {
        out.insert(QStringLiteral("text"), query);
        out.insert(QStringLiteral("visible"), !matches.isEmpty());
    } else {
        out.insert(QStringLiteral("query"), query);
    }
    return jsonResult(out);
}

ToolResult toolFindText(const QJsonObject& args) {
    return textQuery(args, false);
}

ToolResult toolAssertText(const QJsonObject& args) {
    return textQuery(args, true);
}

qint64 clampMs(const QJsonObject& args, const QString& key, qint64 def, qint64 lo, qint64 hi) {
    const qint64 value = static_cast<qint64>(args.value(key).toDouble(static_cast<double>(def)));
    return std::min(std::max(value, lo), hi);
}

ToolResult toolWaitForText(const QJsonObject& args) {
    const QString query = args.value(QStringLiteral("text")).toString().trimmed();
    if (query.isEmpty()) {
        return errorResult(QStringLiteral("text is required"));
    }
    const qint64 timeout_ms = clampMs(args, QStringLiteral("timeout_ms"), 10'000, 200, 30'000);
    const qint64 poll_ms = clampMs(args, QStringLiteral("poll_ms"), 500, 150, 5000);
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        CaptureRequest req{};
        QString err;
        QJsonArray matches;
        if (resolveTextTarget(args, req, err)) {  // window not there yet -> keep waiting
            QVector<WordHit> hits;
            const QString ocr_err = ocrCollect(req, hits);
            if (!ocr_err.isEmpty()) {
                return errorResult(ocr_err);  // an engine problem is not a "not yet"
            }
            matches = matchesToJson(hits, query);
        }
        const qint64 waited = static_cast<qint64>(GetTickCount64() - start);
        if (!matches.isEmpty()) {
            return jsonResult(QJsonObject{{QStringLiteral("found"), true},
                                          {QStringLiteral("waited_ms"), waited},
                                          {QStringLiteral("count"), matches.size()},
                                          {QStringLiteral("matches"), matches}});
        }
        if (waited >= timeout_ms) {
            return jsonResult(QJsonObject{{QStringLiteral("found"), false},
                                          {QStringLiteral("waited_ms"), waited},
                                          {QStringLiteral("count"), 0}});
        }
        Sleep(static_cast<DWORD>(poll_ms));
    }
}

void appendTextTools(QJsonArray& tools) {
    const QJsonObject text_target =
        QJsonObject{{QStringLiteral("text"),
                     stringProperty(QStringLiteral("Substring to find (case-insensitive)."))},
                    {QStringLiteral("window_title"),
                     stringProperty(QStringLiteral("Optional window to scope to; omit for the "
                                                   "full screen."))}};
    tools.append(toolEntry(
        QStringLiteral("find_text_on_screen"),
        QStringLiteral("OCR the screen (or a window) and return every occurrence of the query as a "
                       "word with its absolute screen box (x/y/width/height), for locating text to "
                       "click."),
        toolSchema(text_target, QJsonArray{QStringLiteral("text")})));
    tools.append(toolEntry(
        QStringLiteral("assert_text_visible"),
        QStringLiteral("Check whether text is currently visible on screen (or in a window) via "
                       "OCR. Returns visible=true/false plus any matching boxes."),
        toolSchema(text_target, QJsonArray{QStringLiteral("text")})));
    QJsonObject wait_props = text_target;
    wait_props.insert(QStringLiteral("timeout_ms"),
                      intProperty(QStringLiteral("Max wait (default 10000, max 30000).")));
    wait_props.insert(QStringLiteral("poll_ms"),
                      intProperty(QStringLiteral("Poll interval (default 500, min 150).")));
    tools.append(toolEntry(
        QStringLiteral("wait_for_text"),
        QStringLiteral("Poll with OCR until text appears on screen (or in a window) or the timeout "
                       "elapses. Returns found=true/false and waited_ms."),
        toolSchema(wait_props, QJsonArray{QStringLiteral("text")})));
}

void appendRegionTools(QJsonArray& tools) {
    const QJsonObject region_schema = toolSchema(
        QJsonObject{{QStringLiteral("x"), intProperty(QStringLiteral("Left edge (screen)."))},
                    {QStringLiteral("y"), intProperty(QStringLiteral("Top edge (screen)."))},
                    {QStringLiteral("width"), intProperty(QStringLiteral("Region width."))},
                    {QStringLiteral("height"), intProperty(QStringLiteral("Region height."))}},
        QJsonArray{QStringLiteral("x"),
                   QStringLiteral("y"),
                   QStringLiteral("width"),
                   QStringLiteral("height")});
    tools.append(toolEntry(
        QStringLiteral("ocr_region"),
        QStringLiteral("Read visible text in a screen rectangle via Windows OCR. Returns the text "
                       "and line count."),
        region_schema));
    tools.append(toolEntry(
        QStringLiteral("ocr_region_structured"),
        QStringLiteral("Like ocr_region but also returns each word with its absolute screen "
                       "bounding box (x/y/width/height), for locating text to click."),
        region_schema));
}

void appendOcrTools(QJsonArray& tools) {
    tools.append(toolEntry(
        QStringLiteral("ocr_window"),
        QStringLiteral("Read visible text from a window (matched by title substring) via Windows "
                       "OCR -- text the UI Automation tree cannot expose (canvas, images, "
                       "custom-drawn UI). Returns text and line count."),
        toolSchema(QJsonObject{{QStringLiteral("window_title"),
                                stringProperty(QStringLiteral("Title substring to match."))}},
                   QJsonArray{QStringLiteral("window_title")})));
    appendRegionTools(tools);
    tools.append(toolEntry(QStringLiteral("ocr_screen"),
                           QStringLiteral(
                               "Read visible text across the full virtual screen via Windows OCR."),
                           toolSchema(QJsonObject{}, QJsonArray{})));
    tools.append(toolEntry(
        QStringLiteral("ocr_screen_structured"),
        QStringLiteral("Like ocr_screen but also returns each word with its absolute screen "
                       "bounding box (x/y/width/height)."),
        toolSchema(QJsonObject{}, QJsonArray{})));
    appendTextTools(tools);
}

struct OcrHandler {
    QLatin1String name;
    ToolResult (*fn)(const QJsonObject&);
};

const OcrHandler kOcrHandlers[] = {
    {QLatin1String("ocr_window"), toolOcrWindow},
    {QLatin1String("ocr_region"), toolOcrRegion},
    {QLatin1String("ocr_region_structured"), toolOcrRegionStructured},
    {QLatin1String("ocr_screen"), toolOcrScreen},
    {QLatin1String("ocr_screen_structured"), toolOcrScreenStructured},
    {QLatin1String("find_text_on_screen"), toolFindText},
    {QLatin1String("assert_text_visible"), toolAssertText},
    {QLatin1String("wait_for_text"), toolWaitForText},
};

}  // namespace

QJsonArray ocrToolCatalog() {
    QJsonArray tools;
    appendOcrTools(tools);
    return tools;
}

bool ocrHandles(const QString& name) {
    for (const auto& entry : kOcrHandlers) {
        if (name == entry.name) {
            return true;
        }
    }
    return false;
}

ToolResult invokeOcrTool(const QString& name, const QJsonObject& args) {
    for (const auto& entry : kOcrHandlers) {
        if (name == entry.name) {
            return entry.fn(args);
        }
    }
    return errorResult(QStringLiteral("Unknown OCR tool: %1").arg(name));
}

}  // namespace sak::win32mcp
