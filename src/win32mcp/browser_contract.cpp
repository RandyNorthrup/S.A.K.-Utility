// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/browser_contract.h"

#include <QHash>
#include <QVector>

namespace sak::win32mcp::browser {

namespace {

// -- Snapshot rendering ------------------------------------------------------

// Cap element names so one absurdly long label cannot blow up the outline the
// model has to read; keep it ASCII (no ellipsis glyph) per the repo text rule.
constexpr int kMaxNameChars = 120;
// Guard against a hostile/broken capture reporting a huge depth and producing a
// megabyte of leading spaces.
constexpr int kMaxIndentDepth = 20;
// Cap how many nodes we render. The whole capture is an untrusted, web-page-derived
// payload; without this a hostile page reporting millions of nodes would blow up the
// outline string + ref_index held in the long-lived process and overflow next_ref.
constexpr int kMaxNodes = 4000;
// Cap the page-controlled url/title that head the snapshot text.
constexpr int kMaxUrlChars = 2048;
constexpr int kMaxTitleChars = 300;

// Collapse to a single line and length-cap an untrusted string. simplified() strips
// newlines/tabs, which is what stops a hostile role/title/url from forging extra
// outline lines or fake [ref=...] entries in the model-facing text.
QString oneLine(const QString& raw, int cap) {
    QString value = raw.simplified();
    if (value.size() > cap) {
        value = value.left(cap) + QLatin1String("...");
    }
    return value;
}

QString escapeName(const QString& raw) {
    QString name = raw.simplified();  // collapse internal whitespace + trim
    name.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    name.replace(QLatin1Char('"'), QLatin1String("\\\""));
    if (name.size() > kMaxNameChars) {
        name = name.left(kMaxNameChars) + QLatin1String("...");
    }
    return name;
}

// A backendNodeId is only usable if it is a positive JSON integer. A hostile capture
// could report it as a string/object/array/float to smuggle a non-integer into the
// CDP command envelope; such a node gets no ref (it cannot be acted on).
bool asBackendId(const QJsonValue& value, qint64* out) {
    if (!value.isDouble()) {
        return false;
    }
    const double raw = value.toDouble();
    if (raw < 1.0 || raw > 9.0e15) {  // stay within exact-integer double range
        return false;
    }
    const auto id = static_cast<qint64>(raw);
    if (static_cast<double>(id) != raw) {  // reject non-integral values
        return false;
    }
    *out = id;
    return true;
}

// An interactable role we still show even with no accessible name, because the
// model may need to act on it (e.g. an icon-only button).
bool roleIsStructuralNoise(const QString& role) {
    return role.isEmpty() || role == QLatin1String("generic") || role == QLatin1String("none") ||
           role == QLatin1String("presentation") || role == QLatin1String("inlinetextbox");
}

// Drop invisible nodes, zero-area nodes, and unnamed structural filler. A node
// with no bounds object is kept (the extension may omit bounds for virtualized
// content); only an explicit zero/negative size is treated as not rendered.
bool nodeIsRenderable(const QJsonObject& node, bool interactable, const QString& name) {
    if (!node.value(QStringLiteral("visible")).toBool(true)) {
        return false;
    }
    if (node.contains(QStringLiteral("bounds"))) {
        const QJsonObject bounds = node.value(QStringLiteral("bounds")).toObject();
        if (bounds.value(QStringLiteral("width")).toInt() <= 0 ||
            bounds.value(QStringLiteral("height")).toInt() <= 0) {
            return false;
        }
    }
    if (!interactable && name.isEmpty() &&
        roleIsStructuralNoise(node.value(QStringLiteral("role")).toString())) {
        return false;
    }
    return true;
}

// A node gets an actionable [ref] if it is interactable, OR if it exposes an inspectable value
// -- a progress bar, meter, output, or any value/range-bearing node. Without this, a read-only
// display (e.g. a <progress> or role=progressbar) has no ref, so browser_get_value /
// get_attribute / box cannot target it even though the model may need to read it.
bool nodeIsRefWorthy(const QJsonObject& node, bool interactable) {
    if (interactable || node.contains(QStringLiteral("value"))) {
        return true;
    }
    return node.contains(QStringLiteral("valuemin")) && node.contains(QStringLiteral("valuemax"));
}

// Append the current value + range for inputs/sliders. The value is page-derived, so it is
// collapsed to one line and stripped of the paren/comma delimiters that could otherwise forge
// extra state flags in the outline.
void appendValueFlags(const QJsonObject& node, QStringList& flags) {
    if (node.contains(QStringLiteral("value"))) {
        QString value = oneLine(node.value(QStringLiteral("value")).toString(), 60);
        value.remove(QLatin1Char('(')).remove(QLatin1Char(')')).remove(QLatin1Char(','));
        if (!value.isEmpty()) {
            flags << QStringLiteral("value=") + value;
        }
    }
    if (node.contains(QStringLiteral("valuemin")) && node.contains(QStringLiteral("valuemax"))) {
        flags << QStringLiteral("range %1..%2")
                     .arg(node.value(QStringLiteral("valuemin")).toDouble())
                     .arg(node.value(QStringLiteral("valuemax")).toDouble());
    }
}

QString stateSuffix(const QJsonObject& node) {
    QStringList flags;
    // Tri-state flags rendered as one of two labels when present.
    struct TriFlag {
        const char* key;
        const char* on;
        const char* off;
    };
    static const TriFlag kTri[] = {{"checked", "checked", "unchecked"},
                                   {"expanded", "expanded", "collapsed"}};
    for (const auto& t : kTri) {
        if (node.contains(QLatin1String(t.key))) {
            flags << QLatin1String(node.value(QLatin1String(t.key)).toBool() ? t.on : t.off);
        }
    }
    // Simple boolean flags whose label is the key.
    static const char* const kBools[] = {
        "editable", "disabled", "selected", "pressed", "readonly", "required", "invalid", "busy"};
    for (const char* key : kBools) {
        if (node.value(QLatin1String(key)).toBool()) {
            flags << QLatin1String(key);
        }
    }
    appendValueFlags(node, flags);
    return flags.isEmpty()
               ? QString()
               : QStringLiteral("(") + flags.join(QLatin1Char(',')) + QStringLiteral(")");
}

QString buildNodeLine(const QJsonObject& node, const QString& name, const QString& ref) {
    int depth = node.value(QStringLiteral("depth")).toInt(0);
    depth = qBound(0, depth, kMaxIndentDepth);
    // role is untrusted: escape + cap it like name, so a hostile role cannot inject
    // newlines/brackets/quotes and forge fake outline lines or [ref=...] entries.
    QString role = escapeName(node.value(QStringLiteral("role")).toString());
    if (role.isEmpty()) {
        role = QStringLiteral("generic");
    }
    QString line = QString(depth * 2, QLatin1Char(' ')) + QStringLiteral("- ") + role;
    if (!name.isEmpty()) {
        line += QStringLiteral(" \"") + name + QLatin1Char('"');
    }
    if (!ref.isEmpty()) {
        line += QStringLiteral(" [ref=") + ref + QLatin1Char(']');
    }
    const QString suffix = stateSuffix(node);
    if (!suffix.isEmpty()) {
        line += QLatin1Char(' ') + suffix;
    }
    return line + QLatin1Char('\n');
}

// -- Command translation -----------------------------------------------------

struct ArgSpec {
    QString key;
    QString type;  // "string" | "int" | "number" | "bool"
    bool required{false};
};

struct CmdSpec {
    QString cmd;
    QString ref_mode;  // "none" | "optional" | "required"
    QVector<ArgSpec> args;
};

// Navigation, read, and element-interaction command specs.
QHash<QString, CmdSpec> interactionCommandSpecs() {
    return {
        {QStringLiteral("browser_navigate"),
         {QStringLiteral("navigate"),
          QStringLiteral("none"),
          {{QStringLiteral("url"), QStringLiteral("string"), true}}}},
        {QStringLiteral("browser_snapshot"),
         {QStringLiteral("snapshot"), QStringLiteral("none"), {}}},
        {QStringLiteral("browser_click"),
         {QStringLiteral("click"),
          QStringLiteral("required"),
          {{QStringLiteral("button"), QStringLiteral("string"), false},
           {QStringLiteral("click_count"), QStringLiteral("int"), false},
           {QStringLiteral("modifiers"), QStringLiteral("string"), false}}}},
        {QStringLiteral("browser_hover"),
         {QStringLiteral("hover"),
          QStringLiteral("required"),
          {{QStringLiteral("duration_ms"), QStringLiteral("int"), false},
           {QStringLiteral("modifiers"), QStringLiteral("string"), false}}}},
        {QStringLiteral("browser_drag"),
         {QStringLiteral("drag"),
          QStringLiteral("optional"),
          {{QStringLiteral("from_x"), QStringLiteral("int"), false},
           {QStringLiteral("from_y"), QStringLiteral("int"), false},
           {QStringLiteral("to_x"), QStringLiteral("int"), false},
           {QStringLiteral("to_y"), QStringLiteral("int"), false},
           {QStringLiteral("steps"), QStringLiteral("int"), false},
           {QStringLiteral("hold_ms"), QStringLiteral("int"), false}}}},
        {QStringLiteral("browser_type"),
         {QStringLiteral("type"),
          QStringLiteral("required"),
          {{QStringLiteral("text"), QStringLiteral("string"), true},
           {QStringLiteral("submit"), QStringLiteral("bool"), false}}}},
        {QStringLiteral("browser_press_key"),
         {QStringLiteral("pressKey"),
          QStringLiteral("none"),
          {{QStringLiteral("keys"), QStringLiteral("string"), true}}}},
        {QStringLiteral("browser_scroll"),
         {QStringLiteral("scroll"),
          QStringLiteral("optional"),
          {{QStringLiteral("direction"), QStringLiteral("string"), false},
           {QStringLiteral("amount"), QStringLiteral("int"), false}}}},
        {QStringLiteral("browser_click_at"),
         {QStringLiteral("clickAt"),
          QStringLiteral("none"),
          {{QStringLiteral("x"), QStringLiteral("int"), true},
           {QStringLiteral("y"), QStringLiteral("int"), true},
           {QStringLiteral("button"), QStringLiteral("string"), false},
           {QStringLiteral("click_count"), QStringLiteral("int"), false},
           {QStringLiteral("modifiers"), QStringLiteral("string"), false}}}},
        {QStringLiteral("browser_dialog"),
         {QStringLiteral("dialog"),
          QStringLiteral("none"),
          {{QStringLiteral("action"), QStringLiteral("string"), false},
           {QStringLiteral("text"), QStringLiteral("string"), false}}}},
    };
}

// Form-control command specs (dropdown, value-set, media).
QHash<QString, CmdSpec> formCommandSpecs() {
    return {
        {QStringLiteral("browser_select"),
         {QStringLiteral("select"),
          QStringLiteral("required"),
          {{QStringLiteral("value"), QStringLiteral("string"), false},
           {QStringLiteral("label"), QStringLiteral("string"), false},
           {QStringLiteral("index"), QStringLiteral("int"), false}}}},
        {QStringLiteral("browser_set_value"),
         {QStringLiteral("setValue"),
          QStringLiteral("required"),
          {{QStringLiteral("value"), QStringLiteral("string"), false},
           {QStringLiteral("checked"), QStringLiteral("bool"), false}}}},
        {QStringLiteral("browser_media"),
         {QStringLiteral("media"),
          QStringLiteral("required"),
          {{QStringLiteral("action"), QStringLiteral("string"), true},
           {QStringLiteral("value"), QStringLiteral("string"), false}}}},
    };
}

// History, read/screenshot, and tab/tab-group command specs.
QHash<QString, CmdSpec> pageAndTabCommandSpecs() {
    return {
        {QStringLiteral("browser_back"), {QStringLiteral("back"), QStringLiteral("none"), {}}},
        {QStringLiteral("browser_forward"),
         {QStringLiteral("forward"), QStringLiteral("none"), {}}},
        {QStringLiteral("browser_reload"), {QStringLiteral("reload"), QStringLiteral("none"), {}}},
        {QStringLiteral("browser_read"),
         {QStringLiteral("read"),
          QStringLiteral("none"),
          {{QStringLiteral("format"), QStringLiteral("string"), false}}}},
        {QStringLiteral("browser_screenshot"),
         {QStringLiteral("screenshot"),
          QStringLiteral("none"),
          {{QStringLiteral("full_page"), QStringLiteral("bool"), false}}}},
        {QStringLiteral("browser_tabs"), {QStringLiteral("listTabs"), QStringLiteral("none"), {}}},
        {QStringLiteral("browser_select_tab"),
         {QStringLiteral("selectTab"),
          QStringLiteral("none"),
          {{QStringLiteral("index"), QStringLiteral("int"), true}}}},
        {QStringLiteral("browser_new_tab"),
         {QStringLiteral("newTab"),
          QStringLiteral("none"),
          {{QStringLiteral("url"), QStringLiteral("string"), false}}}},
        {QStringLiteral("browser_close_tab"),
         {QStringLiteral("closeTab"),
          QStringLiteral("none"),
          {{QStringLiteral("index"), QStringLiteral("int"), false}}}},
        {QStringLiteral("browser_group_tabs"),
         {QStringLiteral("groupTabs"),
          QStringLiteral("none"),
          {{QStringLiteral("tab_indices"), QStringLiteral("string"), false},
           {QStringLiteral("title"), QStringLiteral("string"), false},
           {QStringLiteral("color"), QStringLiteral("string"), false}}}},
        {QStringLiteral("browser_ungroup_tabs"),
         {QStringLiteral("ungroupTabs"),
          QStringLiteral("none"),
          {{QStringLiteral("tab_indices"), QStringLiteral("string"), false}}}},
    };
}

// Read-only inspection + robustness command specs (no page mutation).
QHash<QString, CmdSpec> inspectionCommandSpecs() {
    return {
        {QStringLiteral("browser_wait_for"),
         {QStringLiteral("waitFor"),
          QStringLiteral("none"),
          {{QStringLiteral("text"), QStringLiteral("string"), false},
           {QStringLiteral("url_contains"), QStringLiteral("string"), false},
           {QStringLiteral("selector"), QStringLiteral("string"), false},
           {QStringLiteral("network_idle"), QStringLiteral("bool"), false},
           {QStringLiteral("idle_ms"), QStringLiteral("int"), false},
           {QStringLiteral("absent"), QStringLiteral("bool"), false},
           {QStringLiteral("timeout_ms"), QStringLiteral("int"), false}}}},
        {QStringLiteral("browser_get_value"),
         {QStringLiteral("getValue"), QStringLiteral("required"), {}}},
        {QStringLiteral("browser_get_attribute"),
         {QStringLiteral("getAttribute"),
          QStringLiteral("required"),
          {{QStringLiteral("name"), QStringLiteral("string"), false}}}},
        {QStringLiteral("browser_box"), {QStringLiteral("box"), QStringLiteral("required"), {}}},
        {QStringLiteral("browser_focus"),
         {QStringLiteral("focus"), QStringLiteral("required"), {}}},
        {QStringLiteral("browser_reveal"),
         {QStringLiteral("reveal"), QStringLiteral("required"), {}}},
    };
}

// Advanced (gated) input command specs.
QHash<QString, CmdSpec> advancedInputCommandSpecs() {
    return {
        {QStringLiteral("browser_js_click"),
         {QStringLiteral("jsClick"), QStringLiteral("required"), {}}},
    };
}

// Gated infrastructure command specs (Batch 4).
QHash<QString, CmdSpec> infraCommandSpecs() {
    return {
        {QStringLiteral("browser_emulate"),
         {QStringLiteral("emulate"),
          QStringLiteral("none"),
          {{QStringLiteral("width"), QStringLiteral("int"), false},
           {QStringLiteral("height"), QStringLiteral("int"), false},
           {QStringLiteral("device_scale_factor"), QStringLiteral("int"), false},
           {QStringLiteral("mobile"), QStringLiteral("bool"), false},
           {QStringLiteral("user_agent"), QStringLiteral("string"), false},
           {QStringLiteral("touch"), QStringLiteral("bool"), false},
           {QStringLiteral("reset"), QStringLiteral("bool"), false}}}},
        {QStringLiteral("browser_print"),
         {QStringLiteral("print"),
          QStringLiteral("none"),
          {{QStringLiteral("landscape"), QStringLiteral("bool"), false},
           {QStringLiteral("scale"), QStringLiteral("number"), false},
           {QStringLiteral("paper_width"), QStringLiteral("number"), false},
           {QStringLiteral("paper_height"), QStringLiteral("number"), false},
           {QStringLiteral("print_background"), QStringLiteral("bool"), false},
           {QStringLiteral("page_ranges"), QStringLiteral("string"), false}}}},
    };
}

// Browser-window (chrome.windows) command specs.
QHash<QString, CmdSpec> windowCommandSpecs() {
    return {
        {QStringLiteral("browser_windows"),
         {QStringLiteral("listWindows"), QStringLiteral("none"), {}}},
        {QStringLiteral("browser_window"),
         {QStringLiteral("window"),
          QStringLiteral("none"),
          {{QStringLiteral("action"), QStringLiteral("string"), true},
           {QStringLiteral("window_id"), QStringLiteral("int"), false},
           {QStringLiteral("url"), QStringLiteral("string"), false}}}},
    };
}

const QHash<QString, CmdSpec>& commandSpecs() {
    static const QHash<QString, CmdSpec> specs = [] {
        QHash<QString, CmdSpec> merged = interactionCommandSpecs();
        merged.insert(formCommandSpecs());
        merged.insert(pageAndTabCommandSpecs());
        merged.insert(inspectionCommandSpecs());
        merged.insert(advancedInputCommandSpecs());
        merged.insert(windowCommandSpecs());
        merged.insert(infraCommandSpecs());
        return merged;
    }();
    return specs;
}

// Resolve an optional/required `ref` argument to a backendNodeId via the current
// snapshot index. Returns an error string (empty on success) and, on success with
// a present ref, writes backendNodeId into `command`.
QString resolveRef(const QJsonObject& args,
                   const QJsonObject& ref_index,
                   bool required,
                   QJsonObject& command) {
    const QString ref = args.value(QStringLiteral("ref")).toString();
    if (ref.isEmpty()) {
        return required ? QStringLiteral("ref is required") : QString();
    }
    if (!ref_index.contains(ref)) {
        return QStringLiteral("Unknown element ref '%1'; call browser_snapshot to refresh")
            .arg(ref);
    }
    command.insert(QStringLiteral("backendNodeId"),
                   ref_index.value(ref).toObject().value(QStringLiteral("backendNodeId")));
    return QString();
}

QString copyArg(const QJsonObject& args, const ArgSpec& spec, QJsonObject& command) {
    if (!args.contains(spec.key)) {
        return spec.required ? QStringLiteral("%1 is required").arg(spec.key) : QString();
    }
    const QJsonValue value = args.value(spec.key);
    if (spec.type == QLatin1String("int")) {
        command.insert(spec.key, value.toInt());
    } else if (spec.type == QLatin1String("number")) {
        command.insert(spec.key, value.toDouble());
    } else if (spec.type == QLatin1String("bool")) {
        command.insert(spec.key, value.toBool());
    } else {
        command.insert(spec.key, value.toString());
    }
    return QString();
}

ExtensionCommand fail(const QString& reason) {
    return {QJsonObject{}, false, reason};
}

// Tool-specific extras the generic ref/scalar-arg copiers do not cover: browser_drag's second
// ref endpoint (to_ref) and browser_select's array-valued multi-select values (passed through
// verbatim). Returns an error string (empty on success).
QString applyToolExtras(const QString& tool,
                        const QJsonObject& arguments,
                        const QJsonObject& ref_index,
                        QJsonObject& command) {
    if (tool == QLatin1String("browser_drag")) {
        const QString to_ref = arguments.value(QStringLiteral("to_ref")).toString();
        if (to_ref.isEmpty()) {
            return QString();
        }
        if (!ref_index.contains(to_ref)) {
            return QStringLiteral("Unknown element ref '%1'; call browser_snapshot to refresh")
                .arg(to_ref);
        }
        command.insert(QStringLiteral("to_backendNodeId"),
                       ref_index.value(to_ref).toObject().value(QStringLiteral("backendNodeId")));
        return QString();
    }
    if (tool == QLatin1String("browser_select") &&
        arguments.value(QStringLiteral("values")).isArray()) {
        command.insert(QStringLiteral("values"), arguments.value(QStringLiteral("values")));
    }
    return QString();
}

// -- Catalog -----------------------------------------------------------------

QJsonObject stringProperty(const QString& description) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                       {QStringLiteral("description"), description}};
}

QJsonObject typedProperty(const QString& type, const QString& description) {
    return QJsonObject{{QStringLiteral("type"), type},
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

void appendNavTools(QJsonArray& tools) {
    tools.append(
        toolEntry(QStringLiteral("browser_navigate"),
                  QStringLiteral("Navigate the active tab to a URL."),
                  toolSchema(QJsonObject{{QStringLiteral("url"),
                                          stringProperty(QStringLiteral("Absolute URL to open."))}},
                             QJsonArray{QStringLiteral("url")})));
    tools.append(toolEntry(
        QStringLiteral("browser_snapshot"),
        QStringLiteral("Capture a filtered DOM tree of the active tab: interactable elements "
                       "with a stable [ref] used by browser_click / browser_type."),
        toolSchema({}, {})));
    tools.append(toolEntry(QStringLiteral("browser_back"),
                           QStringLiteral("Go back one entry in the active tab's history."),
                           toolSchema({}, {})));
    tools.append(toolEntry(QStringLiteral("browser_forward"),
                           QStringLiteral("Go forward one entry in the active tab's history."),
                           toolSchema({}, {})));
    tools.append(toolEntry(QStringLiteral("browser_reload"),
                           QStringLiteral("Reload the active tab."),
                           toolSchema({}, {})));
    tools.append(
        toolEntry(QStringLiteral("browser_read"),
                  QStringLiteral("Read the active tab's content as text (default) or html."),
                  toolSchema(QJsonObject{{QStringLiteral("format"),
                                          stringProperty(QStringLiteral("\"text\" or \"html\"."))}},
                             {})));
    tools.append(toolEntry(
        QStringLiteral("browser_screenshot"),
        QStringLiteral("Capture a PNG screenshot of the active tab (viewport by default)."),
        toolSchema(QJsonObject{{QStringLiteral("full_page"),
                                typedProperty(QStringLiteral("boolean"),
                                              QStringLiteral("Capture the full scroll height."))}},
                   {})));
}

void appendActionTools(QJsonArray& tools) {
    tools.append(toolEntry(
        QStringLiteral("browser_click"),
        QStringLiteral("Click the element with [ref] from the latest snapshot (injected at the "
                       "browser level, so the user's mouse is untouched). button is left "
                       "(default), right (context menu), or middle; click_count 2 double-clicks "
                       "and 3 triple-clicks; modifiers like \"Control+Shift\" enable multi/range "
                       "select."),
        toolSchema(
            QJsonObject{{QStringLiteral("ref"),
                         stringProperty(QStringLiteral("Element ref, e.g. \"e5\"."))},
                        {QStringLiteral("button"),
                         stringProperty(QStringLiteral("left | right | middle (optional)."))},
                        {QStringLiteral("click_count"),
                         typedProperty(QStringLiteral("integer"),
                                       QStringLiteral("1, 2, or 3 (optional)."))},
                        {QStringLiteral("modifiers"),
                         stringProperty(QStringLiteral("e.g. \"Control\", \"Shift\" "
                                                       "(optional)."))}},
            QJsonArray{QStringLiteral("ref")})));
    tools.append(toolEntry(
        QStringLiteral("browser_type"),
        QStringLiteral("Type text into the element with [ref] from the latest snapshot. Set submit "
                       "to press Enter afterward."),
        toolSchema(QJsonObject{{QStringLiteral("text"),
                                stringProperty(QStringLiteral("Text to type."))},
                               {QStringLiteral("ref"),
                                stringProperty(QStringLiteral("Target element ref, e.g. \"e5\"."))},
                               {QStringLiteral("submit"),
                                typedProperty(QStringLiteral("boolean"),
                                              QStringLiteral("Press Enter after typing."))}},
                   QJsonArray{QStringLiteral("text"), QStringLiteral("ref")})));
    tools.append(toolEntry(
        QStringLiteral("browser_press_key"),
        QStringLiteral("Press a key or chord, e.g. \"Enter\", \"Escape\", \"Control+A\"."),
        toolSchema(QJsonObject{{QStringLiteral("keys"),
                                stringProperty(QStringLiteral("Key or chord to press."))}},
                   QJsonArray{QStringLiteral("keys")})));
    tools.append(
        toolEntry(QStringLiteral("browser_scroll"),
                  QStringLiteral("Scroll the page, or the element with [ref] if given."),
                  toolSchema(QJsonObject{{QStringLiteral("ref"),
                                          stringProperty(
                                              QStringLiteral("Element ref to scroll (optional)."))},
                                         {QStringLiteral("direction"),
                                          stringProperty(QStringLiteral("\"up\" or \"down\"."))},
                                         {QStringLiteral("amount"),
                                          typedProperty(QStringLiteral("integer"),
                                                        QStringLiteral("Pixels to scroll."))}},
                             {})));
    tools.append(toolEntry(
        QStringLiteral("browser_dialog"),
        QStringLiteral("Set how the NEXT JavaScript dialog is answered and report the last one. "
                       "Dialogs are auto-answered (alert/beforeunload accepted, confirm/prompt "
                       "dismissed); call this with action \"accept\" just before the step that "
                       "opens a confirm/prompt you want to accept."),
        toolSchema(QJsonObject{{QStringLiteral("action"),
                                stringProperty(QStringLiteral("\"accept\" or \"dismiss\" "
                                                              "(default) for the next dialog."))},
                               {QStringLiteral("text"),
                                stringProperty(QStringLiteral("Text for an accepted prompt() "
                                                              "dialog (optional)."))}},
                   {})));
}

void appendPointerTools(QJsonArray& tools) {
    tools.append(toolEntry(
        QStringLiteral("browser_click_at"),
        QStringLiteral("Click at pixel coordinates read from the most recent browser_screenshot "
                       "(x,y in that image's pixels). Use this only for targets with no [ref] "
                       "(canvas, image maps); prefer browser_click by ref otherwise. Supports "
                       "button, click_count, and modifiers like browser_click."),
        toolSchema(
            QJsonObject{{QStringLiteral("x"),
                         typedProperty(QStringLiteral("integer"),
                                       QStringLiteral("X in screenshot pixels."))},
                        {QStringLiteral("y"),
                         typedProperty(QStringLiteral("integer"),
                                       QStringLiteral("Y in screenshot pixels."))},
                        {QStringLiteral("button"),
                         stringProperty(QStringLiteral("left | right | middle (optional)."))},
                        {QStringLiteral("click_count"),
                         typedProperty(QStringLiteral("integer"),
                                       QStringLiteral("1, 2, or 3 (optional)."))},
                        {QStringLiteral("modifiers"),
                         stringProperty(QStringLiteral("e.g. \"Control+Shift\" (optional)."))}},
            QJsonArray{QStringLiteral("x"), QStringLiteral("y")})));
    tools.append(toolEntry(
        QStringLiteral("browser_hover"),
        QStringLiteral("Move the pointer over the element with [ref] to reveal hover menus, "
                       "tooltips, or row actions, then take a fresh snapshot to read what "
                       "appeared. duration_ms lets hover-intent UI settle."),
        toolSchema(
            QJsonObject{{QStringLiteral("ref"),
                         stringProperty(QStringLiteral("Element ref to hover, e.g. \"e5\"."))},
                        {QStringLiteral("duration_ms"),
                         typedProperty(QStringLiteral("integer"),
                                       QStringLiteral("Dwell time in ms (optional)."))}},
            QJsonArray{QStringLiteral("ref")})));
    tools.append(toolEntry(
        QStringLiteral("browser_drag"),
        QStringLiteral("Drag from a source to a target with interpolated moves (sliders, "
                       "sortable/kanban lists, splitters, canvas, media scrubbers). Give the "
                       "source as [ref] or from_x/from_y and the target as to_ref or to_x/to_y. "
                       "hold_ms pauses after press for long-press pickup."),
        toolSchema(
            QJsonObject{
                {QStringLiteral("ref"),
                 stringProperty(QStringLiteral("Source element ref (optional if from_x/from_y)."))},
                {QStringLiteral("to_ref"),
                 stringProperty(QStringLiteral("Target element ref (optional if to_x/to_y)."))},
                {QStringLiteral("from_x"),
                 typedProperty(QStringLiteral("integer"), QStringLiteral("Source X (optional)."))},
                {QStringLiteral("from_y"),
                 typedProperty(QStringLiteral("integer"), QStringLiteral("Source Y (optional)."))},
                {QStringLiteral("to_x"),
                 typedProperty(QStringLiteral("integer"), QStringLiteral("Target X (optional)."))},
                {QStringLiteral("to_y"),
                 typedProperty(QStringLiteral("integer"), QStringLiteral("Target Y (optional)."))},
                {QStringLiteral("steps"),
                 typedProperty(QStringLiteral("integer"),
                               QStringLiteral("Interpolated moves (optional)."))},
                {QStringLiteral("hold_ms"),
                 typedProperty(QStringLiteral("integer"),
                               QStringLiteral("Hold after press (optional)."))}},
            {})));
}

void appendFormControlTools(QJsonArray& tools) {
    tools.append(toolEntry(
        QStringLiteral("browser_select"),
        QStringLiteral(
            "Select an option in a <select> dropdown with [ref] from the latest snapshot, "
            "by option value, visible label, or zero-based index (give exactly one). "
            "Fires the page's input and change events."),
        toolSchema(
            QJsonObject{
                {QStringLiteral("ref"),
                 stringProperty(QStringLiteral("The <select> element ref, e.g. \"e5\"."))},
                {QStringLiteral("value"),
                 stringProperty(QStringLiteral("Option value attribute to select (optional)."))},
                {QStringLiteral("label"),
                 stringProperty(QStringLiteral("Visible option text to select (optional)."))},
                {QStringLiteral("index"),
                 typedProperty(QStringLiteral("integer"),
                               QStringLiteral("Zero-based option index (optional)."))},
                {QStringLiteral("values"),
                 QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                             {QStringLiteral("items"),
                              QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
                             {QStringLiteral("description"),
                              QStringLiteral(
                                  "Option values or labels to select in a multiple-select "
                                  "(optional).")}}}},
            QJsonArray{QStringLiteral("ref")})));
    tools.append(toolEntry(
        QStringLiteral("browser_set_value"),
        QStringLiteral(
            "Set the value of a form control with [ref] from the latest snapshot and fire "
            "its input/change events. Works for range sliders, date/time/color/number "
            "inputs, and hidden or custom controls; pass checked (true/false) for a "
            "checkbox or radio. Use browser_type for normal text fields."),
        toolSchema(
            QJsonObject{{QStringLiteral("ref"),
                         stringProperty(QStringLiteral("Target control ref, e.g. \"e5\"."))},
                        {QStringLiteral("value"),
                         stringProperty(QStringLiteral("Value to set (e.g. \"7\", \"2026-07-29\", "
                                                       "\"#ff2d95\")."))},
                        {QStringLiteral("checked"),
                         typedProperty(QStringLiteral("boolean"),
                                       QStringLiteral("For a checkbox/radio: checked state."))}},
            QJsonArray{QStringLiteral("ref")})));
    tools.append(toolEntry(
        QStringLiteral("browser_media"),
        QStringLiteral("Control an <audio> or <video> element with [ref] from the latest snapshot. "
                       "action is play, pause, mute, unmute, seek, volume, or rate; value is the "
                       "seconds (seek), 0..1 (volume), or playback rate. Returns the media state."),
        toolSchema(
            QJsonObject{
                {QStringLiteral("ref"),
                 stringProperty(QStringLiteral("The media element ref, e.g. \"e5\"."))},
                {QStringLiteral("action"),
                 stringProperty(QStringLiteral("play | pause | mute | unmute | seek | volume | "
                                               "rate."))},
                {QStringLiteral("value"),
                 stringProperty(
                     QStringLiteral("Numeric argument for seek/volume/rate (optional)."))}},
            QJsonArray{QStringLiteral("ref"), QStringLiteral("action")})));
}

void appendTabTools(QJsonArray& tools) {
    tools.append(toolEntry(QStringLiteral("browser_tabs"),
                           QStringLiteral("List the open tabs with index, title, and URL."),
                           toolSchema({}, {})));
    tools.append(
        toolEntry(QStringLiteral("browser_select_tab"),
                  QStringLiteral("Make the tab at the given index active."),
                  toolSchema(QJsonObject{{QStringLiteral("index"),
                                          typedProperty(QStringLiteral("integer"),
                                                        QStringLiteral("Zero-based tab index."))}},
                             QJsonArray{QStringLiteral("index")})));
    tools.append(toolEntry(
        QStringLiteral("browser_new_tab"),
        QStringLiteral("Open a new tab, optionally navigating to a URL."),
        toolSchema(QJsonObject{{QStringLiteral("url"),
                                stringProperty(QStringLiteral("URL to open (optional)."))}},
                   {})));
    tools.append(toolEntry(
        QStringLiteral("browser_close_tab"),
        QStringLiteral("Close the tab at index, or the active tab if omitted."),
        toolSchema(QJsonObject{{QStringLiteral("index"),
                                typedProperty(QStringLiteral("integer"),
                                              QStringLiteral("Zero-based tab index (optional)."))}},
                   {})));
    tools.append(toolEntry(
        QStringLiteral("browser_group_tabs"),
        QStringLiteral("Group tabs into a Chrome tab group (creating one), optionally naming and "
                       "coloring it. tab_indices is a comma-separated list of zero-based tab "
                       "indices; the active tab is grouped if omitted."),
        toolSchema(
            QJsonObject{
                {QStringLiteral("tab_indices"),
                 stringProperty(QStringLiteral("Comma-separated zero-based tab indices, e.g. "
                                               "\"0,1,2\" (optional; default the active tab)."))},
                {QStringLiteral("title"), stringProperty(QStringLiteral("Group name (optional)."))},
                {QStringLiteral("color"),
                 stringProperty(QStringLiteral("Group color: grey, blue, red, yellow, green, pink, "
                                               "purple, cyan, or orange (optional)."))}},
            {})));
    tools.append(toolEntry(
        QStringLiteral("browser_ungroup_tabs"),
        QStringLiteral("Remove tabs from their Chrome tab group. tab_indices is a comma-separated "
                       "list of zero-based tab indices; the active tab is ungrouped if omitted."),
        toolSchema(QJsonObject{{QStringLiteral("tab_indices"),
                                stringProperty(QStringLiteral(
                                    "Comma-separated zero-based tab indices (optional)."))}},
                   {})));
}

void appendWaitForTool(QJsonArray& tools) {
    tools.append(toolEntry(
        QStringLiteral("browser_wait_for"),
        QStringLiteral("Wait until a condition holds on the active tab, or timeout. Give exactly "
                       "one of text (page text appears), url_contains (URL contains a substring), "
                       "selector (a CSS selector matches, pierces shadow DOM), or network_idle "
                       "(no in-flight network requests for idle_ms). Set absent to wait for a "
                       "text/url/selector condition to go away instead. Read-only polling."),
        toolSchema(
            QJsonObject{
                {QStringLiteral("text"),
                 stringProperty(QStringLiteral("Page text to wait for (optional)."))},
                {QStringLiteral("url_contains"),
                 stringProperty(QStringLiteral("Substring the tab URL must contain (optional)."))},
                {QStringLiteral("selector"),
                 stringProperty(QStringLiteral("CSS selector to wait for (optional)."))},
                {QStringLiteral("network_idle"),
                 typedProperty(QStringLiteral("boolean"),
                               QStringLiteral("Wait until network activity settles (optional)."))},
                {QStringLiteral("idle_ms"),
                 typedProperty(QStringLiteral("integer"),
                               QStringLiteral(
                                   "Quiet window for network_idle in ms (default 500)."))},
                {QStringLiteral("absent"),
                 typedProperty(QStringLiteral("boolean"),
                               QStringLiteral("Wait for the condition to be false (optional)."))},
                {QStringLiteral("timeout_ms"),
                 typedProperty(QStringLiteral("integer"),
                               QStringLiteral("Max wait in ms (default 8000)."))}},
            {})));
}

void appendInspectionTools(QJsonArray& tools) {
    tools.append(toolEntry(
        QStringLiteral("browser_get_value"),
        QStringLiteral("Read the current state of a control with [ref]: value, checked, selected "
                       "options, contenteditable text, and disabled. Read-only."),
        toolSchema(
            QJsonObject{{QStringLiteral("ref"),
                         stringProperty(QStringLiteral("Target control ref, e.g. \"e5\"."))}},
            QJsonArray{QStringLiteral("ref")})));
    tools.append(toolEntry(
        QStringLiteral("browser_get_attribute"),
        QStringLiteral("Read an element's attributes by [ref]: give name for one attribute "
                       "(e.g. href, src, aria-label), or omit name for all attributes. Read-only."),
        toolSchema(QJsonObject{{QStringLiteral("ref"),
                                stringProperty(QStringLiteral("Target element ref, e.g. \"e5\"."))},
                               {QStringLiteral("name"),
                                stringProperty(
                                    QStringLiteral("Attribute name (optional; all if omitted)."))}},
                   QJsonArray{QStringLiteral("ref")})));
    tools.append(toolEntry(
        QStringLiteral("browser_box"),
        QStringLiteral("Report an element's geometry by [ref]: position/size, whether it is in "
                       "the viewport, and whether an overlay covers its center (occlusion). "
                       "Read-only."),
        toolSchema(
            QJsonObject{{QStringLiteral("ref"),
                         stringProperty(QStringLiteral("Target element ref, e.g. \"e5\"."))}},
            QJsonArray{QStringLiteral("ref")})));
    tools.append(toolEntry(
        QStringLiteral("browser_focus"),
        QStringLiteral("Focus the element with [ref] (to then browser_press_key into it without "
                       "clicking)."),
        toolSchema(
            QJsonObject{{QStringLiteral("ref"),
                         stringProperty(QStringLiteral("Target element ref, e.g. \"e5\"."))}},
            QJsonArray{QStringLiteral("ref")})));
    tools.append(
        toolEntry(QStringLiteral("browser_reveal"),
                  QStringLiteral("Scroll the element with [ref] into view (before a screenshot or "
                                 "browser_click_at)."),
                  toolSchema(QJsonObject{{QStringLiteral("ref"),
                                          stringProperty(
                                              QStringLiteral("Target element ref, e.g. \"e5\"."))}},
                             QJsonArray{QStringLiteral("ref")})));
}

void appendWindowTools(QJsonArray& tools) {
    tools.append(toolEntry(
        QStringLiteral("browser_windows"),
        QStringLiteral("List the open browser windows (window_id, focused, type, state, tab_count) "
                       "so a model-opened popup or new window is visible. Read-only."),
        toolSchema({}, {})));
    tools.append(toolEntry(
        QStringLiteral("browser_window"),
        QStringLiteral("Manage a browser window. action is new (open a window, optionally at a "
                       "URL), focus (bring window_id to front), or close (close window_id). Get "
                       "window_id from browser_windows."),
        toolSchema(
            QJsonObject{{QStringLiteral("action"),
                         stringProperty(QStringLiteral("new | focus | close."))},
                        {QStringLiteral("window_id"),
                         typedProperty(QStringLiteral("integer"),
                                       QStringLiteral("Target window id for focus/close."))},
                        {QStringLiteral("url"),
                         stringProperty(QStringLiteral("URL to open for action new (optional)."))}},
            QJsonArray{QStringLiteral("action")})));
}

void appendInfraTools(QJsonArray& tools) {
    tools.append(toolEntry(
        QStringLiteral("browser_emulate"),
        QStringLiteral("Emulate a device on the active tab for responsive/mobile/UA testing: give "
                       "width+height (CSS px, with optional device_scale_factor and mobile), a "
                       "user_agent string, and/or touch. Pass reset:true to clear all emulation "
                       "and restore the real device. Overrides also clear when you switch tabs."),
        toolSchema(
            QJsonObject{{QStringLiteral("width"),
                         typedProperty(QStringLiteral("integer"),
                                       QStringLiteral("Viewport width in CSS "
                                                      "px (with height)."))},
                        {QStringLiteral("height"),
                         typedProperty(QStringLiteral("integer"),
                                       QStringLiteral("Viewport height in CSS "
                                                      "px (with width)."))},
                        {QStringLiteral("device_scale_factor"),
                         typedProperty(QStringLiteral("integer"),
                                       QStringLiteral(
                                           "Device pixel ratio, e.g. 2 (optional, default 1)."))},
                        {QStringLiteral("mobile"),
                         typedProperty(QStringLiteral("boolean"),
                                       QStringLiteral(
                                           "Emulate a mobile device (meta viewport, optional)."))},
                        {QStringLiteral("user_agent"),
                         stringProperty(QStringLiteral("User-Agent string to send (optional)."))},
                        {QStringLiteral("touch"),
                         typedProperty(QStringLiteral("boolean"),
                                       QStringLiteral("Enable touch input emulation (optional)."))},
                        {QStringLiteral("reset"),
                         typedProperty(QStringLiteral("boolean"),
                                       QStringLiteral(
                                           "Clear all emulation and restore the real device."))}},
            {})));
}

void appendPrintTool(QJsonArray& tools) {
    tools.append(toolEntry(
        QStringLiteral("browser_print"),
        QStringLiteral(
            "Print the active tab to a PDF file (no printer, no OS dialog) and save it "
            "to the user's Downloads/SAK-Utility folder; returns the saved path. Options: "
            "landscape, scale (0.1-2), paper_width/paper_height (inches), print_background "
            "(default true), and page_ranges like \"1-3\"."),
        toolSchema(
            QJsonObject{
                {QStringLiteral("landscape"),
                 typedProperty(QStringLiteral("boolean"),
                               QStringLiteral("Landscape orientation (optional)."))},
                {QStringLiteral("scale"),
                 typedProperty(QStringLiteral("number"),
                               QStringLiteral("Render scale 0.1-2 (optional, default 1)."))},
                {QStringLiteral("paper_width"),
                 typedProperty(QStringLiteral("number"),
                               QStringLiteral("Paper width in inches (optional)."))},
                {QStringLiteral("paper_height"),
                 typedProperty(QStringLiteral("number"),
                               QStringLiteral("Paper height in inches (optional)."))},
                {QStringLiteral("print_background"),
                 typedProperty(QStringLiteral("boolean"),
                               QStringLiteral("Print CSS backgrounds (optional, "
                                              "default true)."))},
                {QStringLiteral("page_ranges"),
                 stringProperty(QStringLiteral("Pages to print, e.g. \"1-3\" (optional)."))}},
            {})));
}

void appendAdvancedInputTools(QJsonArray& tools) {
    tools.append(toolEntry(
        QStringLiteral("browser_js_click"),
        QStringLiteral("Click the element with [ref] via its DOM click() method, as a fallback "
                       "when a normal browser_click cannot land (target occluded by an overlay, "
                       "zero-size but present, or ignoring synthetic pointer events). Prefer "
                       "browser_click otherwise."),
        toolSchema(
            QJsonObject{{QStringLiteral("ref"),
                         stringProperty(QStringLiteral("Target element ref, e.g. \"e5\"."))}},
            QJsonArray{QStringLiteral("ref")})));
}

// Cap how many omitted-frame URLs are listed so a page spawning hundreds of cross-origin
// iframes cannot flood the outline.
constexpr int kMaxOmittedFramesListed = 20;

// Cross-origin (out-of-process) iframes are not reached by the single-pass capture. Rather
// than a bare "some content is missing", list the omitted frames' URLs so the model can
// navigate into a frame it needs. Each URL is page-derived, so it is collapsed to one line
// and length-capped (a newline in a URL could otherwise forge an outline line); refs are
// only ever trusted from ref_index, never parsed from this text. Falls back to the older
// boolean-only note when an extension does not send the URL list.
void appendOmittedFramesNote(const QJsonObject& capture, QString& outline) {
    const QJsonArray frames = capture.value(QStringLiteral("omittedFrames")).toArray();
    if (!frames.isEmpty()) {
        outline += QStringLiteral(
            "  ... (cross-origin iframe content not included; "
            "browser_navigate to a frame URL to read it):\n");
        int listed = 0;
        for (const QJsonValue& value : frames) {
            if (listed >= kMaxOmittedFramesListed) {
                outline += QStringLiteral("    ... (more omitted frames)\n");
                break;
            }
            const QString url = oneLine(value.toString(), kMaxUrlChars);
            if (url.isEmpty()) {
                continue;
            }
            outline += QStringLiteral("    - %1\n").arg(url);
            ++listed;
        }
        return;
    }
    if (capture.value(QStringLiteral("iframesOmitted")).toBool()) {
        outline += QStringLiteral(
            "  ... (cross-origin iframe content is not included in this snapshot)\n");
    }
}

}  // namespace

SnapshotView renderSnapshot(const QJsonObject& capture) {
    SnapshotView view;
    view.url = oneLine(capture.value(QStringLiteral("url")).toString(), kMaxUrlChars);
    view.title = oneLine(capture.value(QStringLiteral("title")).toString(), kMaxTitleChars);

    QString outline;
    QJsonObject ref_index;
    int next_ref = 0;
    int emitted = 0;
    // The extension caps its own node walk and marks the capture truncated; honor that
    // marker so the model is told the outline is partial even though our local emitted
    // count never reaches kMaxNodes for an already-capped capture. A page-derived bool
    // consumed only to append a fixed literal note -- no injection surface.
    bool truncated = capture.value(QStringLiteral("truncated")).toBool();
    const QJsonArray nodes = capture.value(QStringLiteral("nodes")).toArray();
    for (const QJsonValue& value : nodes) {
        const QJsonObject node = value.toObject();
        const bool interactable = node.value(QStringLiteral("interactable")).toBool();
        const QString name = escapeName(node.value(QStringLiteral("name")).toString());
        if (!nodeIsRenderable(node, interactable, name)) {
            continue;
        }
        if (emitted >= kMaxNodes) {
            truncated = true;
            break;
        }
        // An interactable OR value-bearing node with a valid integer backendNodeId gets a ref;
        // otherwise it is shown for context but cannot be acted on.
        qint64 backend_id = 0;
        QString ref;
        if (nodeIsRefWorthy(node, interactable) &&
            asBackendId(node.value(QStringLiteral("backendNodeId")), &backend_id)) {
            ref = QStringLiteral("e%1").arg(++next_ref);
            ref_index.insert(ref,
                             QJsonObject{{QStringLiteral("backendNodeId"), backend_id},
                                         {QStringLiteral("role"),
                                          node.value(QStringLiteral("role")).toString()},
                                         {QStringLiteral("name"), name}});
        }
        outline += buildNodeLine(node, name, ref);
        ++emitted;
    }
    if (truncated) {
        outline += QStringLiteral("  ... (more elements omitted; the page is very large)\n");
    }
    // The single-pass capture does not reach cross-origin (out-of-process) iframes; when the
    // extension reports them, name the omitted frames so the model can navigate into them
    // rather than treat the snapshot as the whole page.
    appendOmittedFramesNote(capture, outline);

    view.outline = outline;
    view.ref_index = ref_index;
    view.element_count = next_ref;
    return view;
}

QJsonArray browserToolCatalog() {
    QJsonArray tools;
    appendNavTools(tools);
    appendActionTools(tools);
    appendPointerTools(tools);
    appendFormControlTools(tools);
    appendTabTools(tools);
    appendWaitForTool(tools);
    appendInspectionTools(tools);
    appendWindowTools(tools);
    appendInfraTools(tools);
    appendPrintTool(tools);
    appendAdvancedInputTools(tools);
    return tools;
}

ExtensionCommand buildExtensionCommand(const QString& tool,
                                       const QJsonObject& arguments,
                                       const QJsonObject& ref_index) {
    const auto& specs = commandSpecs();
    const auto it = specs.constFind(tool);
    if (it == specs.constEnd()) {
        return fail(QStringLiteral("Unknown browser tool: %1").arg(tool));
    }
    const CmdSpec& spec = it.value();
    QJsonObject command{{QStringLiteral("cmd"), spec.cmd}};
    if (spec.ref_mode != QLatin1String("none")) {
        const QString error =
            resolveRef(arguments, ref_index, spec.ref_mode == QLatin1String("required"), command);
        if (!error.isEmpty()) {
            return fail(error);
        }
    }
    for (const ArgSpec& arg : spec.args) {
        const QString error = copyArg(arguments, arg, command);
        if (!error.isEmpty()) {
            return fail(error);
        }
    }
    // Tool-specific extras (drag's second ref endpoint, array-valued upload/select args).
    const QString extra = applyToolExtras(tool, arguments, ref_index, command);
    if (!extra.isEmpty()) {
        return fail(extra);
    }
    return {command, true, QString()};
}

}  // namespace sak::win32mcp::browser
