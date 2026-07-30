// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_desktop.h"

#include "sak/win32mcp/win32_mcp_capture.h"

#include <QBuffer>
#include <QByteArray>
#include <QImage>
#include <QJsonDocument>
#include <QStringList>
#include <QVector>

#include <algorithm>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// WIN32_LEAN_AND_MEAN drops the core COM/OLE headers windows.h would otherwise pull, but
// <uiautomation.h> needs the `interface` keyword + COM base types (objbase) and BSTR/VARIANT
// helpers (oleauto) to declare its provider interfaces; include them explicitly first.
#include <objbase.h>
#include <oleauto.h>
#include <uiautomation.h>
#include <wrl/client.h>

namespace sak::win32mcp {

namespace {

// -- result + schema helpers (module-local, mirroring win32_mcp_tools) -------

ToolResult jsonResult(const QJsonObject& object) {
    return {QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)), false};
}

ToolResult errorResult(const QString& message) {
    return {QJsonDocument(QJsonObject{{QStringLiteral("error"), message}})
                .toJson(QJsonDocument::Compact),
            true};
}

ToolResult imageResult(const QString& base64, const QString& summary) {
    ToolResult result;
    result.text = summary;
    result.is_error = false;
    result.image_base64 = base64;
    result.image_mime = QStringLiteral("image/png");
    return result;
}

QJsonObject stringProperty(const QString& description) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                       {QStringLiteral("description"), description}};
}

QJsonObject typedProperty(const QString& type, const QString& description) {
    return QJsonObject{{QStringLiteral("type"), type},
                       {QStringLiteral("description"), description}};
}

QJsonObject boolProperty(const QString& description) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")},
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

// -- window lookup -----------------------------------------------------------

QString windowTitleOf(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    if (length <= 0) {
        return {};
    }
    QVector<wchar_t> buffer(length + 1, L'\0');
    const int copied = GetWindowTextW(hwnd, buffer.data(), length + 1);
    return QString::fromWCharArray(buffer.data(), copied);
}

HWND findVisibleWindowByTitle(const QString& needle_lower) {
    struct FindState {
        QString needle_lower;
        HWND match{nullptr};
    } state{needle_lower, nullptr};
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto* find = reinterpret_cast<FindState*>(param);
            if (IsWindowVisible(hwnd) == FALSE) {
                return TRUE;
            }
            const QString title = windowTitleOf(hwnd);
            if (title.isEmpty() || !title.toLower().contains(find->needle_lower)) {
                return TRUE;
            }
            find->match = hwnd;
            return FALSE;  // stop at the first match
        },
        reinterpret_cast<LPARAM>(&state));
    return state.match;
}

// Resolve the window an observation tool targets: foreground=true -> the active window (the only
// way to reach a title-less pop-up dialog), otherwise the first visible window whose title
// contains window_title. Sets err and returns null on failure.
HWND resolveTargetHwnd(const QJsonObject& args, QString& err) {
    if (args.value(QStringLiteral("foreground")).toBool()) {
        HWND fg = GetForegroundWindow();
        if (!fg) {
            err = QStringLiteral("There is no foreground window.");
        }
        return fg;
    }
    const QString title = args.value(QStringLiteral("window_title")).toString().trimmed();
    if (title.isEmpty()) {
        err = QStringLiteral("window_title or foreground is required");
        return nullptr;
    }
    HWND hwnd = findVisibleWindowByTitle(title.toLower());
    if (!hwnd) {
        err = QStringLiteral("No visible window matching '%1'").arg(title);
    }
    return hwnd;
}

// -- screen capture ----------------------------------------------------------

// A capture scales with the surface; cap the base64 so a multi-monitor grab cannot flood the
// transport (16 MiB matches the browser screenshot cap). kCaptureFullEdge asks captureBgra for
// full-resolution pixels (no downscale) up to its own hard bound.
constexpr int kMaxCaptureBase64 = 16 * 1024 * 1024;
constexpr int kCaptureFullEdge = 16'384;

// Encode a top-down 32bpp buffer as base64 PNG. Format_RGB32 reads the B,G,R,X bytes a screen
// DIB holds (little-endian) and ignores the unused 4th byte, so an opaque capture round-trips.
QString encodePng(const uchar* bits, int width, int height) {
    const QImage image(bits, width, height, width * 4, QImage::Format_RGB32);
    if (image.isNull()) {
        return {};
    }
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG")) {
        return {};
    }
    return QString::fromLatin1(png.toBase64());
}

// Capture a window (hwnd non-null -> PrintWindow) or screen rect to a base64 PNG via the shared
// grab primitive. Returns empty and sets *err on failure or an over-cap result.
QString captureToBase64(void* hwnd, const RECT& rect, QString* err) {
    CaptureBits bits;
    const CaptureRequest req{hwnd, rect.left, rect.top, rect.right, rect.bottom, kCaptureFullEdge};
    if (!captureBgra(req, bits, *err)) {
        return {};
    }
    const QString base64 =
        encodePng(reinterpret_cast<const uchar*>(bits.bgra.constData()), bits.width, bits.height);
    if (base64.isEmpty()) {
        *err = QStringLiteral("Failed to encode the capture as PNG.");
        return {};
    }
    if (base64.size() > kMaxCaptureBase64) {
        *err = QStringLiteral("The capture is too large to return; target one window/monitor.");
        return {};
    }
    return base64;
}

ToolResult toolCaptureWindow(const QJsonObject& args) {
    QString err_target;
    HWND hwnd = resolveTargetHwnd(args, err_target);
    if (!hwnd) {
        return errorResult(err_target);
    }
    if (IsIconic(hwnd)) {
        return errorResult(QStringLiteral("The window is minimized; restore it before capturing."));
    }
    RECT rc{};
    if (GetWindowRect(hwnd, &rc) == FALSE) {
        return errorResult(QStringLiteral("Could not read the window bounds."));
    }
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    QString err;
    const QString b64 = captureToBase64(hwnd, rc, &err);
    if (b64.isEmpty()) {
        return errorResult(err);
    }
    return imageResult(b64,
                       QStringLiteral("Captured a %1x%2 PNG of window '%3'.")
                           .arg(w)
                           .arg(h)
                           .arg(windowTitleOf(hwnd)));
}

ToolResult toolCaptureScreen(const QJsonObject&) {
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    QString err;
    const RECT rc{x, y, x + w, y + h};
    const QString b64 = captureToBase64(nullptr, rc, &err);
    if (b64.isEmpty()) {
        return errorResult(err);
    }
    return imageResult(
        b64, QStringLiteral("Captured a %1x%2 PNG of the full virtual screen.").arg(w).arg(h));
}

BOOL CALLBACK collectMonitorRectProc(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    auto* rects = reinterpret_cast<QVector<RECT>*>(param);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) != FALSE) {
        rects->append(info.rcMonitor);
    }
    return TRUE;
}

ToolResult toolCaptureMonitor(const QJsonObject& args) {
    if (!args.contains(QStringLiteral("index"))) {
        return errorResult(QStringLiteral("index is required (see list_monitors)"));
    }
    const int index = args.value(QStringLiteral("index")).toInt(-1);
    QVector<RECT> rects;
    EnumDisplayMonitors(nullptr, nullptr, collectMonitorRectProc, reinterpret_cast<LPARAM>(&rects));
    if (index < 0 || index >= rects.size()) {
        return errorResult(
            QStringLiteral("No monitor at index %1 (there are %2).").arg(index).arg(rects.size()));
    }
    const RECT rc = rects[index];
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    QString err;
    const QString b64 = captureToBase64(nullptr, rc, &err);
    if (b64.isEmpty()) {
        return errorResult(err);
    }
    return imageResult(
        b64, QStringLiteral("Captured a %1x%2 PNG of monitor %3.").arg(w).arg(h).arg(index));
}

// -- UI Automation (read) ----------------------------------------------------
//
// A read-only view of a window's live control tree via IUIAutomation: an outline the model
// reads (mirroring browser_snapshot's role/name/value + ref model) plus targeted lookups.
// Refs are the depth-first index into the last inspected tree and are valid only against THAT
// window; a ref action re-walks the same window and refuses if the tree drifted, so a stale
// ref can never read the wrong control. No input is injected here -- clicking/invoking a
// control is a separate hard-gated input tool in a later batch.

using Microsoft::WRL::ComPtr;

// One control-tree node, flattened depth-first. `role` is a friendly control-type name.
struct UiaNode {
    QString role;
    QString name;
    QString value;
    int depth{0};
    bool enabled{true};
    bool offscreen{false};
};

// Bound the walk so a pathological tree (thousands of list items) cannot flood the transport
// or spin unbounded; kDefaultUiaDepth also caps a caller-supplied max_depth.
constexpr int kMaxUiaNodes = 400;
constexpr int kDefaultUiaDepth = 40;
constexpr int kMaxUiaNameChars = 200;
constexpr int kMaxIndentDepth = 20;

// COM apartment scoped to one tool call. UIA lives in the MTA; tolerate a thread that is
// already an STA (CoInitializeEx returns RPC_E_CHANGED_MODE) by not un-initializing what we
// did not initialize -- the existing apartment still services CoCreateInstance.
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

QString bstrToQString(BSTR text) {
    if (text == nullptr) {
        return {};
    }
    return QString::fromWCharArray(text, static_cast<int>(SysStringLen(text)));
}

// Map the common UIA control-type ids to short, stable, lower-case names. Anything unlisted
// reports "control" so the outline is still well-formed.
const char* controlTypeName(CONTROLTYPEID id) {
    struct Row {
        int id;
        const char* name;
    };
    static const Row kRows[] = {
        {UIA_ButtonControlTypeId, "button"},
        {UIA_CheckBoxControlTypeId, "checkbox"},
        {UIA_ComboBoxControlTypeId, "combobox"},
        {UIA_EditControlTypeId, "edit"},
        {UIA_HyperlinkControlTypeId, "link"},
        {UIA_ImageControlTypeId, "image"},
        {UIA_ListItemControlTypeId, "listitem"},
        {UIA_ListControlTypeId, "list"},
        {UIA_MenuControlTypeId, "menu"},
        {UIA_MenuBarControlTypeId, "menubar"},
        {UIA_MenuItemControlTypeId, "menuitem"},
        {UIA_ProgressBarControlTypeId, "progressbar"},
        {UIA_RadioButtonControlTypeId, "radio"},
        {UIA_ScrollBarControlTypeId, "scrollbar"},
        {UIA_SliderControlTypeId, "slider"},
        {UIA_SpinnerControlTypeId, "spinner"},
        {UIA_StatusBarControlTypeId, "statusbar"},
        {UIA_TabControlTypeId, "tab"},
        {UIA_TabItemControlTypeId, "tabitem"},
        {UIA_TextControlTypeId, "text"},
        {UIA_ToolBarControlTypeId, "toolbar"},
        {UIA_ToolTipControlTypeId, "tooltip"},
        {UIA_TreeControlTypeId, "tree"},
        {UIA_TreeItemControlTypeId, "treeitem"},
        {UIA_GroupControlTypeId, "group"},
        {UIA_DataGridControlTypeId, "datagrid"},
        {UIA_DataItemControlTypeId, "dataitem"},
        {UIA_DocumentControlTypeId, "document"},
        {UIA_SplitButtonControlTypeId, "splitbutton"},
        {UIA_WindowControlTypeId, "window"},
        {UIA_PaneControlTypeId, "pane"},
        {UIA_HeaderControlTypeId, "header"},
        {UIA_HeaderItemControlTypeId, "headeritem"},
        {UIA_TableControlTypeId, "table"},
        {UIA_TitleBarControlTypeId, "titlebar"},
        {UIA_SeparatorControlTypeId, "separator"},
    };
    for (const auto& row : kRows) {
        if (row.id == id) {
            return row.name;
        }
    }
    return "control";
}

// Read a control's ValuePattern text, if it exposes one and it is non-empty.
QString readValuePattern(IUIAutomationElement* element) {
    ComPtr<IUnknown> unknown;
    if (FAILED(element->GetCurrentPattern(UIA_ValuePatternId, &unknown)) || !unknown) {
        return {};
    }
    ComPtr<IUIAutomationValuePattern> pattern;
    if (FAILED(unknown.As(&pattern)) || !pattern) {
        return {};
    }
    BSTR value = nullptr;
    if (FAILED(pattern->get_CurrentValue(&value)) || value == nullptr) {
        return {};
    }
    const QString out = bstrToQString(value);
    SysFreeString(value);
    return out;
}

// Read a control's TogglePattern state as on/off/indeterminate, if it exposes one.
QString readToggleState(IUIAutomationElement* element) {
    ComPtr<IUnknown> unknown;
    if (FAILED(element->GetCurrentPattern(UIA_TogglePatternId, &unknown)) || !unknown) {
        return {};
    }
    ComPtr<IUIAutomationTogglePattern> pattern;
    if (FAILED(unknown.As(&pattern)) || !pattern) {
        return {};
    }
    ToggleState state = ToggleState_Off;
    if (FAILED(pattern->get_CurrentToggleState(&state))) {
        return {};
    }
    if (state == ToggleState_On) {
        return QStringLiteral("on");
    }
    if (state == ToggleState_Indeterminate) {
        return QStringLiteral("indeterminate");
    }
    return QStringLiteral("off");
}

// A control's most useful single value: its editable/selected text if any, else its toggle
// state. Empty when the control carries neither (a plain button, label, or group).
QString readElementValue(IUIAutomationElement* element) {
    const QString value = readValuePattern(element);
    if (!value.isEmpty()) {
        return value;
    }
    return readToggleState(element);
}

UiaNode describeElement(IUIAutomationElement* element, int depth) {
    UiaNode node;
    node.depth = depth;
    CONTROLTYPEID control_type = 0;
    element->get_CurrentControlType(&control_type);
    node.role = QString::fromLatin1(controlTypeName(control_type));
    BSTR name = nullptr;
    if (SUCCEEDED(element->get_CurrentName(&name)) && name != nullptr) {
        node.name = bstrToQString(name).trimmed().left(kMaxUiaNameChars);
        SysFreeString(name);
    }
    BOOL enabled = TRUE;
    element->get_CurrentIsEnabled(&enabled);
    node.enabled = enabled != FALSE;
    BOOL offscreen = FALSE;
    element->get_CurrentIsOffscreen(&offscreen);
    node.offscreen = offscreen != FALSE;
    node.value = readElementValue(element);
    return node;
}

struct WalkState {
    QVector<UiaNode>* nodes;
    IUIAutomationTreeWalker* walker;
    int max_depth;
    bool truncated;
    // Optional parallel list of the live elements (same index as nodes), retained so a ref can be
    // invoked. Null when only the read-only outline is needed (inspect/find/get_value).
    QVector<ComPtr<IUIAutomationElement>>* elements;
};

// Depth-first pre-order walk of the control view, appending each element as a flat node. The
// node index becomes its ref. Stops (and flags truncated) once kMaxUiaNodes is reached.
void walkElement(WalkState& state, IUIAutomationElement* element, int depth) {
    if (state.nodes->size() >= kMaxUiaNodes) {
        state.truncated = true;
        return;
    }
    state.nodes->append(describeElement(element, depth));
    if (state.elements != nullptr) {
        state.elements->append(ComPtr<IUIAutomationElement>(element));
    }
    if (depth >= state.max_depth) {
        return;
    }
    ComPtr<IUIAutomationElement> child;
    if (FAILED(state.walker->GetFirstChildElement(element, &child)) || !child) {
        return;
    }
    while (child) {
        walkElement(state, child.Get(), depth + 1);
        if (state.nodes->size() >= kMaxUiaNodes) {
            state.truncated = true;
            return;
        }
        ComPtr<IUIAutomationElement> next;
        if (FAILED(state.walker->GetNextSiblingElement(child.Get(), &next))) {
            return;
        }
        child = next;
    }
}

// Walk `hwnd`'s control tree into `out` (and, if `elements` is non-null, the live element behind
// each node). Precondition: a ComApartment is live on this thread. Returns an error string on any
// COM failure, empty on success.
QString inspectHwnd(HWND hwnd,
                    int max_depth,
                    QVector<UiaNode>& out,
                    bool& truncated,
                    QVector<ComPtr<IUIAutomationElement>>* elements = nullptr) {
    ComPtr<IUIAutomation> automation;
    if (FAILED(CoCreateInstance(
            __uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation))) ||
        !automation) {
        return QStringLiteral("UI Automation is unavailable on this system.");
    }
    ComPtr<IUIAutomationElement> root;
    if (FAILED(automation->ElementFromHandle(hwnd, &root)) || !root) {
        return QStringLiteral("Could not read the window's UI Automation tree.");
    }
    ComPtr<IUIAutomationTreeWalker> walker;
    if (FAILED(automation->get_ControlViewWalker(&walker)) || !walker) {
        return QStringLiteral("The UI Automation control view is unavailable.");
    }
    WalkState state{&out, walker.Get(), max_depth, false, elements};
    walkElement(state, root.Get(), 0);
    truncated = state.truncated;
    return {};
}

QString buildOutline(const QVector<UiaNode>& nodes) {
    QStringList lines;
    lines.reserve(nodes.size());
    for (int i = 0; i < nodes.size(); ++i) {
        const UiaNode& node = nodes[i];
        const QString indent(std::min(node.depth, kMaxIndentDepth) * 2, QLatin1Char(' '));
        QString line = QStringLiteral("%1[%2] %3").arg(indent).arg(i).arg(node.role);
        if (!node.name.isEmpty()) {
            line += QStringLiteral(" \"%1\"").arg(node.name);
        }
        if (!node.value.isEmpty()) {
            line += QStringLiteral(" = \"%1\"").arg(node.value);
        }
        if (!node.enabled) {
            line += QStringLiteral(" (disabled)");
        }
        if (node.offscreen) {
            line += QStringLiteral(" (offscreen)");
        }
        lines << line;
    }
    return lines.join(QLatin1Char('\n'));
}

int clampUiaDepth(const QJsonObject& args) {
    const int requested = args.value(QStringLiteral("max_depth")).toInt(kDefaultUiaDepth);
    if (requested <= 0 || requested > kDefaultUiaDepth) {
        return kDefaultUiaDepth;
    }
    return requested;
}

// The last inspected tree, kept so ref-based lookups can re-walk the same window and verify a
// ref still points at the same control. The native server processes one request at a time, so
// a single module-local snapshot is safe.
struct UiaSnapshot {
    HWND hwnd{nullptr};
    QString title;
    QVector<UiaNode> nodes;
};
UiaSnapshot g_uia_snapshot;

ToolResult toolUiaInspectWindow(const QJsonObject& args) {
    QString err_target;
    HWND hwnd = resolveTargetHwnd(args, err_target);
    if (!hwnd) {
        return errorResult(err_target);
    }
    ComApartment com;
    QVector<UiaNode> nodes;
    bool truncated = false;
    const QString err = inspectHwnd(hwnd, clampUiaDepth(args), nodes, truncated);
    if (!err.isEmpty()) {
        return errorResult(err);
    }
    g_uia_snapshot = UiaSnapshot{hwnd, windowTitleOf(hwnd), nodes};
    return jsonResult(
        QJsonObject{{QStringLiteral("window"), windowTitleOf(hwnd)},
                    {QStringLiteral("hwnd"), QString::number(reinterpret_cast<quintptr>(hwnd))},
                    {QStringLiteral("count"), nodes.size()},
                    {QStringLiteral("truncated"), truncated},
                    {QStringLiteral("outline"), buildOutline(nodes)}});
}

ToolResult toolUiaFindControl(const QJsonObject& args) {
    const QString query = args.value(QStringLiteral("name")).toString().trimmed();
    if (query.isEmpty()) {
        return errorResult(QStringLiteral("name is required"));
    }
    QString err_target;
    HWND hwnd = resolveTargetHwnd(args, err_target);
    if (!hwnd) {
        return errorResult(err_target);
    }
    const QString type_filter =
        args.value(QStringLiteral("control_type")).toString().trimmed().toLower();
    ComApartment com;
    QVector<UiaNode> nodes;
    bool truncated = false;
    const QString err = inspectHwnd(hwnd, kDefaultUiaDepth, nodes, truncated);
    if (!err.isEmpty()) {
        return errorResult(err);
    }
    g_uia_snapshot = UiaSnapshot{hwnd, windowTitleOf(hwnd), nodes};
    const QString needle = query.toLower();
    QJsonArray matches;
    for (int i = 0; i < nodes.size(); ++i) {
        const UiaNode& node = nodes[i];
        if (!node.name.toLower().contains(needle)) {
            continue;
        }
        if (!type_filter.isEmpty() && node.role != type_filter) {
            continue;
        }
        matches.append(QJsonObject{{QStringLiteral("ref"), i},
                                   {QStringLiteral("role"), node.role},
                                   {QStringLiteral("name"), node.name},
                                   {QStringLiteral("value"), node.value},
                                   {QStringLiteral("enabled"), node.enabled}});
    }
    return jsonResult(QJsonObject{{QStringLiteral("window"), windowTitleOf(hwnd)},
                                  {QStringLiteral("count"), matches.size()},
                                  {QStringLiteral("matches"), matches},
                                  {QStringLiteral("tree_truncated"), truncated}});
}

// Guard the stored snapshot before a ref lookup: it must exist and still describe a live
// window with the same title. Returns an error string when stale, empty when usable.
QString uiaSnapshotPrecheck() {
    if (g_uia_snapshot.hwnd == nullptr) {
        return QStringLiteral("No inspection yet; call uia_inspect_window first.");
    }
    if (IsWindow(g_uia_snapshot.hwnd) == FALSE) {
        return QStringLiteral("The inspected window is gone; call uia_inspect_window again.");
    }
    if (windowTitleOf(g_uia_snapshot.hwnd) != g_uia_snapshot.title) {
        return QStringLiteral(
            "The window changed since inspection; call uia_inspect_window again.");
    }
    return {};
}

// True when the ref no longer points at the same control it did at inspection time (the tree
// shifted between calls), so reading it would silently target the wrong element.
bool uiaRefDrifted(const QVector<UiaNode>& live, int ref) {
    if (ref < 0 || ref >= live.size() || ref >= g_uia_snapshot.nodes.size()) {
        return true;
    }
    return live[ref].role != g_uia_snapshot.nodes[ref].role ||
           live[ref].name != g_uia_snapshot.nodes[ref].name;
}

ToolResult toolUiaGetControlValue(const QJsonObject& args) {
    if (!args.contains(QStringLiteral("ref"))) {
        return errorResult(QStringLiteral("ref is required (from uia_inspect_window)"));
    }
    const int ref = args.value(QStringLiteral("ref")).toInt(-1);
    const QString stale = uiaSnapshotPrecheck();
    if (!stale.isEmpty()) {
        return errorResult(stale);
    }
    ComApartment com;
    QVector<UiaNode> nodes;
    bool truncated = false;
    const QString err = inspectHwnd(g_uia_snapshot.hwnd, kDefaultUiaDepth, nodes, truncated);
    if (!err.isEmpty()) {
        return errorResult(err);
    }
    if (uiaRefDrifted(nodes, ref)) {
        return errorResult(
            QStringLiteral("That ref no longer points at the same control; call "
                           "uia_inspect_window again."));
    }
    const UiaNode& node = nodes[ref];
    g_uia_snapshot.nodes = nodes;  // keep the stored tree aligned with what we just read
    return jsonResult(QJsonObject{{QStringLiteral("ref"), ref},
                                  {QStringLiteral("role"), node.role},
                                  {QStringLiteral("name"), node.name},
                                  {QStringLiteral("value"), node.value},
                                  {QStringLiteral("enabled"), node.enabled},
                                  {QStringLiteral("offscreen"), node.offscreen}});
}

// Programmatic activation via UI Automation patterns (no cursor movement): Invoke a button/menu
// item, else Toggle a checkbox, else Select a list/tab item. Each returns true only if the
// control exposes that pattern and the call succeeds.
bool tryInvoke(IUIAutomationElement* element) {
    ComPtr<IUnknown> unknown;
    if (FAILED(element->GetCurrentPattern(UIA_InvokePatternId, &unknown)) || !unknown) {
        return false;
    }
    ComPtr<IUIAutomationInvokePattern> pattern;
    return SUCCEEDED(unknown.As(&pattern)) && pattern && SUCCEEDED(pattern->Invoke());
}

bool tryToggle(IUIAutomationElement* element) {
    ComPtr<IUnknown> unknown;
    if (FAILED(element->GetCurrentPattern(UIA_TogglePatternId, &unknown)) || !unknown) {
        return false;
    }
    ComPtr<IUIAutomationTogglePattern> pattern;
    return SUCCEEDED(unknown.As(&pattern)) && pattern && SUCCEEDED(pattern->Toggle());
}

bool trySelect(IUIAutomationElement* element) {
    ComPtr<IUnknown> unknown;
    if (FAILED(element->GetCurrentPattern(UIA_SelectionItemPatternId, &unknown)) || !unknown) {
        return false;
    }
    ComPtr<IUIAutomationSelectionItemPattern> pattern;
    return SUCCEEDED(unknown.As(&pattern)) && pattern && SUCCEEDED(pattern->Select());
}

QString invokeElement(IUIAutomationElement* element, QString& action) {
    if (tryInvoke(element)) {
        action = QStringLiteral("invoke");
        return {};
    }
    if (tryToggle(element)) {
        action = QStringLiteral("toggle");
        return {};
    }
    if (trySelect(element)) {
        action = QStringLiteral("select");
        return {};
    }
    return QStringLiteral("This control cannot be invoked (no Invoke/Toggle/Select pattern).");
}

ToolResult toolUiaClickControl(const QJsonObject& args) {
    if (!args.contains(QStringLiteral("ref"))) {
        return errorResult(QStringLiteral("ref is required (from uia_inspect_window)"));
    }
    const int ref = args.value(QStringLiteral("ref")).toInt(-1);
    const QString stale = uiaSnapshotPrecheck();
    if (!stale.isEmpty()) {
        return errorResult(stale);
    }
    ComApartment com;
    QVector<UiaNode> nodes;
    bool truncated = false;
    QVector<ComPtr<IUIAutomationElement>> elements;
    const QString err =
        inspectHwnd(g_uia_snapshot.hwnd, kDefaultUiaDepth, nodes, truncated, &elements);
    if (!err.isEmpty()) {
        return errorResult(err);
    }
    if (uiaRefDrifted(nodes, ref) || ref >= elements.size()) {
        return errorResult(
            QStringLiteral("That ref no longer points at the same control; call "
                           "uia_inspect_window again."));
    }
    QString action;
    const QString invoke_err = invokeElement(elements[ref].Get(), action);
    if (!invoke_err.isEmpty()) {
        return errorResult(invoke_err);
    }
    g_uia_snapshot.nodes = nodes;
    const UiaNode& node = nodes[ref];
    return jsonResult(QJsonObject{{QStringLiteral("ok"), true},
                                  {QStringLiteral("ref"), ref},
                                  {QStringLiteral("action"), action},
                                  {QStringLiteral("role"), node.role},
                                  {QStringLiteral("name"), node.name}});
}

QString focusedWindowTitle(IUIAutomationElement* element) {
    UIA_HWND native = nullptr;
    if (FAILED(element->get_CurrentNativeWindowHandle(&native)) || native == nullptr) {
        return {};
    }
    HWND root = GetAncestor(static_cast<HWND>(native), GA_ROOT);
    return windowTitleOf(root != nullptr ? root : static_cast<HWND>(native));
}

ToolResult toolUiaGetFocused(const QJsonObject&) {
    ComApartment com;
    ComPtr<IUIAutomation> automation;
    if (FAILED(CoCreateInstance(
            __uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation))) ||
        !automation) {
        return errorResult(QStringLiteral("UI Automation is unavailable on this system."));
    }
    ComPtr<IUIAutomationElement> element;
    if (FAILED(automation->GetFocusedElement(&element)) || !element) {
        return errorResult(QStringLiteral("No UI element currently has focus."));
    }
    const UiaNode node = describeElement(element.Get(), 0);
    return jsonResult(QJsonObject{{QStringLiteral("role"), node.role},
                                  {QStringLiteral("name"), node.name},
                                  {QStringLiteral("value"), node.value},
                                  {QStringLiteral("enabled"), node.enabled},
                                  {QStringLiteral("window"), focusedWindowTitle(element.Get())}});
}

void appendUiaTools(QJsonArray& tools) {
    tools.append(toolEntry(
        QStringLiteral("uia_inspect_window"),
        QStringLiteral(
            "Read a window's live UI Automation control tree as an indented outline of "
            "[ref] role \"name\" = \"value\" lines (the desktop analog of "
            "browser_snapshot). Each ref is usable with uia_get_control_value. Match the "
            "window by title substring, or set foreground=true to inspect the active pop-up."),
        toolSchema(
            QJsonObject{
                {QStringLiteral("window_title"),
                 stringProperty(QStringLiteral("Title substring of the target window."))},
                {QStringLiteral("foreground"),
                 boolProperty(QStringLiteral("Inspect the active window (title-less pop-ups)."))},
                {QStringLiteral("max_depth"),
                 typedProperty(QStringLiteral("integer"),
                               QStringLiteral("Optional tree depth cap (default/max 40)."))}},
            QJsonArray{})));
    tools.append(toolEntry(
        QStringLiteral("uia_find_control"),
        QStringLiteral(
            "Find controls in a window whose accessible name contains the query "
            "(case-insensitive), optionally filtered by control_type (button, checkbox, "
            "edit, menuitem, tabitem, ...). Returns matching refs with role/name/value."),
        toolSchema(
            QJsonObject{{QStringLiteral("window_title"),
                         stringProperty(QStringLiteral("Title substring of the target window."))},
                        {QStringLiteral("foreground"),
                         boolProperty(QStringLiteral("Search the active window instead."))},
                        {QStringLiteral("name"),
                         stringProperty(QStringLiteral("Accessible-name substring to match."))},
                        {QStringLiteral("control_type"),
                         stringProperty(QStringLiteral("Optional control-type name to require."))}},
            QJsonArray{QStringLiteral("name")})));
    tools.append(toolEntry(
        QStringLiteral("uia_get_control_value"),
        QStringLiteral("Read one control's current value/name/enabled/toggle state by its ref from "
                       "the most recent uia_inspect_window. Re-reads the live control and refuses "
                       "if the tree changed since inspection."),
        toolSchema(
            QJsonObject{{QStringLiteral("ref"),
                         typedProperty(QStringLiteral("integer"),
                                       QStringLiteral("Ref index from uia_inspect_window."))}},
            QJsonArray{QStringLiteral("ref")})));
    tools.append(toolEntry(
        QStringLiteral("uia_get_focused"),
        QStringLiteral("Report the control that currently has keyboard focus (role, name, value, "
                       "and its top-level window), useful for confirming where typed input lands."),
        toolSchema(QJsonObject{}, QJsonArray{})));
    tools.append(toolEntry(
        QStringLiteral("uia_click_control"),
        QStringLiteral("Activate a control by its ref from the most recent uia_inspect_window: "
                       "Invoke a button/menu item, else Toggle a checkbox, else Select a list/tab "
                       "item -- programmatically via UI Automation, WITHOUT moving the mouse. "
                       "Re-checks the tree and refuses if the ref drifted. Preferred over "
                       "click_text/mouse_click when the target has a ref."),
        toolSchema(
            QJsonObject{{QStringLiteral("ref"),
                         typedProperty(QStringLiteral("integer"),
                                       QStringLiteral("Ref index from uia_inspect_window."))}},
            QJsonArray{QStringLiteral("ref")})));
}

// -- catalog + dispatch ------------------------------------------------------

void appendCaptureTools(QJsonArray& tools) {
    tools.append(toolEntry(
        QStringLiteral("capture_window"),
        QStringLiteral("Capture a PNG screenshot of a window (uses PrintWindow, so occluded or "
                       "DWM-composited windows still render). Match by title substring, or set "
                       "foreground=true to grab the active pop-up (which often has no title). "
                       "Fails if the window is minimized."),
        toolSchema(
            QJsonObject{{QStringLiteral("window_title"),
                         stringProperty(QStringLiteral("Title substring to match."))},
                        {QStringLiteral("foreground"),
                         boolProperty(QStringLiteral("Capture the active window instead."))}},
            QJsonArray{})));
    tools.append(toolEntry(
        QStringLiteral("capture_screen"),
        QStringLiteral("Capture a PNG screenshot of the full virtual screen (all monitors)."),
        toolSchema(QJsonObject{}, QJsonArray{})));
    tools.append(toolEntry(
        QStringLiteral("capture_monitor"),
        QStringLiteral("Capture a PNG screenshot of one monitor by its index from list_monitors."),
        toolSchema(
            QJsonObject{{QStringLiteral("index"),
                         typedProperty(QStringLiteral("integer"),
                                       QStringLiteral("Monitor index from list_monitors."))}},
            QJsonArray{QStringLiteral("index")})));
}

struct DesktopHandler {
    QLatin1String name;
    ToolResult (*fn)(const QJsonObject&);
};

const DesktopHandler kDesktopHandlers[] = {
    {QLatin1String("capture_window"), toolCaptureWindow},
    {QLatin1String("capture_screen"), toolCaptureScreen},
    {QLatin1String("capture_monitor"), toolCaptureMonitor},
    {QLatin1String("uia_inspect_window"), toolUiaInspectWindow},
    {QLatin1String("uia_find_control"), toolUiaFindControl},
    {QLatin1String("uia_get_control_value"), toolUiaGetControlValue},
    {QLatin1String("uia_get_focused"), toolUiaGetFocused},
    {QLatin1String("uia_click_control"), toolUiaClickControl},
};

}  // namespace

QJsonArray desktopToolCatalog() {
    QJsonArray tools;
    appendCaptureTools(tools);
    appendUiaTools(tools);
    return tools;
}

bool desktopHandles(const QString& name) {
    for (const auto& entry : kDesktopHandlers) {
        if (name == entry.name) {
            return true;
        }
    }
    return false;
}

ToolResult invokeDesktopTool(const QString& name, const QJsonObject& args) {
    for (const auto& entry : kDesktopHandlers) {
        if (name == entry.name) {
            return entry.fn(args);
        }
    }
    return errorResult(QStringLiteral("Unknown desktop tool: %1").arg(name));
}

}  // namespace sak::win32mcp
