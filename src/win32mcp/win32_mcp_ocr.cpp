// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_ocr.h"

#include "sak/win32mcp/win32_mcp_capture.h"

#include <QJsonDocument>

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

// Shared flow for every OCR tool: create the engine, capture the target (downscaled to the
// engine's limit), recognize, and shape the reply. origin for box mapping is (req.left, top).
ToolResult runOcrCapture(CaptureRequest req, bool structured) {
    ComApartment com;
    const OcrEngine engine = makeEngine();
    if (!engine) {
        return errorResult(QStringLiteral(
            "No OCR language is installed. Add a language with text recognition in Windows "
            "Settings > Time & language > Language."));
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
