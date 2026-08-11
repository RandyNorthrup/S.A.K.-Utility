// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/browser_contract.h"

#include <QHash>
#include <QSet>
#include <QVector>

#include <limits>

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
// Bound the total nodes SCANNED, not just emitted: a hostile capture can pad the array with
// millions of invisible/zero-area nodes that never emit (so the emitted cap never trips) yet
// still force O(N) work. Stop scanning past this and mark the outline truncated. Comfortably
// larger than any real DOM (a huge page is tens of thousands of nodes).
constexpr int kMaxScanNodes = 100'000;
// Cap the page-controlled url/title that head the snapshot text.
constexpr int kMaxUrlChars = 2048;
constexpr int kMaxTitleChars = 300;
// Largest backendNodeId still exactly representable as a double; a larger value
// cannot be an exact integer id and is rejected.
constexpr double kMaxExactIntegerDouble = 9.0e15;
// Cap the page-derived input/slider value string rendered into the outline.
constexpr int kMaxValueChars = 60;
// Spaces of indentation rendered per outline depth level.
constexpr int kSpacesPerIndentLevel = 2;

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
    if (raw < 1.0 || raw > kMaxExactIntegerDouble) {  // stay within exact-integer double range
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
        QString value = oneLine(node.value(QStringLiteral("value")).toString(), kMaxValueChars);
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
        const char* m_key;
        const char* m_on;
        const char* m_off;
    };
    static const TriFlag kTri[] = {{"checked", "checked", "unchecked"},
                                   {"expanded", "expanded", "collapsed"}};
    for (const auto& t : kTri) {
        if (node.contains(QLatin1String(t.m_key))) {
            flags << QLatin1String(node.value(QLatin1String(t.m_key)).toBool() ? t.m_on : t.m_off);
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
    QString line = QString(depth * kSpacesPerIndentLevel, QLatin1Char(' ')) + QStringLiteral("- ") +
                   role;
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
    QString m_key;
    QString m_type;  // "string" | "int" | "number" | "bool"
    bool m_required{false};
};

struct CmdSpec {
    QString m_cmd;
    QString m_ref_mode;  // "none" | "optional" | "required"
    QVector<ArgSpec> m_args;
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

// Gated infrastructure command specs (Batch 4): the ones that reshape or render the PAGE.
QHash<QString, CmdSpec> renderingCommandSpecs() {
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

// Gated infrastructure command specs (Batch 4): the ones that touch ORIGIN-SCOPED state --
// permissions, storage, cookies, downloads and credentials. Each of these acts on whatever tab
// is active when it lands, which is why several carry an explicit origin expectation.
QHash<QString, CmdSpec> infraCommandSpecs() {
    return {
        {QStringLiteral("browser_permission"),
         {QStringLiteral("permission"),
          QStringLiteral("none"),
          {{QStringLiteral("name"), QStringLiteral("string"), true},
           {QStringLiteral("setting"), QStringLiteral("string"), true},
           {QStringLiteral("origin"), QStringLiteral("string"), false}}}},
        {QStringLiteral("browser_storage"),
         {QStringLiteral("storage"),
          QStringLiteral("none"),
          {{QStringLiteral("action"), QStringLiteral("string"), true},
           {QStringLiteral("area"), QStringLiteral("string"), false},
           {QStringLiteral("key"), QStringLiteral("string"), false},
           {QStringLiteral("value"), QStringLiteral("string"), false},
           // The origin the caller BELIEVES it is acting on. Storage is per-origin, and the
           // op targets whatever tab is active when it lands -- a focus change between plan
           // and execution silently retargets it at another site. Declaring the expectation
           // lets the extension refuse instead of reading the wrong origin's storage.
           {QStringLiteral("expect_origin"), QStringLiteral("string"), false}}}},
        {QStringLiteral("browser_cookies"),
         {QStringLiteral("cookies"),
          QStringLiteral("none"),
          {{QStringLiteral("action"), QStringLiteral("string"), true},
           {QStringLiteral("url"), QStringLiteral("string"), false},
           {QStringLiteral("name"), QStringLiteral("string"), false},
           {QStringLiteral("value"), QStringLiteral("string"), false},
           {QStringLiteral("path"), QStringLiteral("string"), false},
           {QStringLiteral("secure"), QStringLiteral("bool"), false},
           {QStringLiteral("http_only"), QStringLiteral("bool"), false},
           {QStringLiteral("same_site"), QStringLiteral("string"), false},
           {QStringLiteral("expires_days"), QStringLiteral("number"), false}}}},
        {QStringLiteral("browser_download"),
         {QStringLiteral("download"),
          QStringLiteral("none"),
          {{QStringLiteral("url"), QStringLiteral("string"), true},
           {QStringLiteral("filename"), QStringLiteral("string"), false},
           {QStringLiteral("timeout_ms"), QStringLiteral("int"), false}}}},
        {QStringLiteral("browser_http_auth"),
         {QStringLiteral("httpAuth"),
          QStringLiteral("none"),
          {{QStringLiteral("username"), QStringLiteral("string"), false},
           {QStringLiteral("password"), QStringLiteral("string"), false},
           {QStringLiteral("clear"), QStringLiteral("bool"), false},
           // Credentials are armed against whichever tab is active at execution time. A
           // popup or focus change between the operator's decision and this call would arm
           // them for an attacker's origin, and the later same-origin check would then
           // validate against that wrong origin. Naming the intended origin is what makes
           // the arming refusable.
           {QStringLiteral("origin"), QStringLiteral("string"), false}}}},
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
    static const QHash<QString, CmdSpec> kSpecs = [] {
        QHash<QString, CmdSpec> merged = interactionCommandSpecs();
        merged.insert(formCommandSpecs());
        merged.insert(pageAndTabCommandSpecs());
        merged.insert(inspectionCommandSpecs());
        merged.insert(advancedInputCommandSpecs());
        merged.insert(windowCommandSpecs());
        merged.insert(renderingCommandSpecs());
        merged.insert(infraCommandSpecs());
        return merged;
    }();
    return kSpecs;
}

// Resolve an optional/required `ref` argument to a backendNodeId via the current
// snapshot index. Returns an error string (empty on success) and, on success with
// a present ref, writes backendNodeId into `command`.
QString resolveRef(const QJsonObject& args,
                   const QJsonObject& ref_index,
                   bool required,
                   QJsonObject& command) {
    const QJsonValue ref_value = args.value(QStringLiteral("ref"));
    // A wrong-typed ref (e.g. a JSON number) must be REJECTED, not silently coerced: toString()
    // on a non-string yields "" which would otherwise look like an absent ref and slip past a
    // required check, or drop an optional ref the caller actually supplied.
    if (args.contains(QStringLiteral("ref")) && !ref_value.isString()) {
        return QStringLiteral("ref must be a string");
    }
    const QString ref = ref_value.toString();
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

// A JSON number is a valid "int" argument only if it is integral and within int32 range. A
// fractional (12.5) or out-of-range (1e18) value is rejected rather than passed to toInt(),
// which truncates the fraction and collapses an out-of-range magnitude to a real coordinate
// (often 0) -- exactly the (0,0)-click hazard the win32 side already guards against.
bool isRepresentableInt(double raw) {
    return raw >= static_cast<double>(std::numeric_limits<int>::min()) &&
           raw <= static_cast<double>(std::numeric_limits<int>::max()) &&
           static_cast<double>(static_cast<int>(raw)) == raw;
}

// Empty on a type match, else an error naming the expected JSON type. A wrong-typed value
// (e.g. a string where an int is required) is rejected rather than coerced: silent coercion
// would turn a malformed "x":"abc" into a real (0,0) command instead of surfacing the error.
QString argTypeMismatch(const QString& key, const QString& type, const QJsonValue& value) {
    if (type == QLatin1String("int")) {
        if (!value.isDouble()) {
            return QStringLiteral("%1 must be a number").arg(key);
        }
        return isRepresentableInt(value.toDouble())
                   ? QString()
                   : QStringLiteral("%1 must be an integer within range").arg(key);
    }
    if (type == QLatin1String("number")) {
        return value.isDouble() ? QString() : QStringLiteral("%1 must be a number").arg(key);
    }
    if (type == QLatin1String("bool")) {
        return value.isBool() ? QString() : QStringLiteral("%1 must be a boolean").arg(key);
    }
    return value.isString() ? QString() : QStringLiteral("%1 must be a string").arg(key);
}

QString copyArg(const QJsonObject& args, const ArgSpec& spec, QJsonObject& command) {
    if (!args.contains(spec.m_key)) {
        return spec.m_required ? QStringLiteral("%1 is required").arg(spec.m_key) : QString();
    }
    const QJsonValue value = args.value(spec.m_key);
    const QString mismatch = argTypeMismatch(spec.m_key, spec.m_type, value);
    if (!mismatch.isEmpty()) {
        return mismatch;
    }
    if (spec.m_type == QLatin1String("int")) {
        command.insert(spec.m_key, value.toInt());
    } else if (spec.m_type == QLatin1String("number")) {
        command.insert(spec.m_key, value.toDouble());
    } else if (spec.m_type == QLatin1String("bool")) {
        command.insert(spec.m_key, value.toBool());
    } else {
        command.insert(spec.m_key, value.toString());
    }
    return QString();
}

ExtensionCommand fail(const QString& reason) {
    return {QJsonObject{}, false, reason};
}

// browser_drag's second ref endpoint (to_ref). A wrong-typed to_ref is rejected rather than
// coerced to "" (which would silently drop a supplied endpoint); an absent one is fine.
QString applyDragExtra(const QJsonObject& arguments,
                       const QJsonObject& ref_index,
                       QJsonObject& command) {
    const QJsonValue to_ref_value = arguments.value(QStringLiteral("to_ref"));
    if (arguments.contains(QStringLiteral("to_ref")) && !to_ref_value.isString()) {
        return QStringLiteral("to_ref must be a string");
    }
    const QString to_ref = to_ref_value.toString();
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

// browser_select's array-valued multi-select values. A present-but-non-array (or an array with
// a non-string entry) is rejected rather than silently ignored, so a malformed multi-select is
// surfaced instead of degrading to a single-value select.
QString applySelectValues(const QJsonObject& arguments, QJsonObject& command) {
    if (!arguments.contains(QStringLiteral("values"))) {
        return QString();
    }
    const QJsonValue values = arguments.value(QStringLiteral("values"));
    if (!values.isArray()) {
        return QStringLiteral("values must be an array of strings");
    }
    for (const QJsonValue& item : values.toArray()) {
        if (!item.isString()) {
            return QStringLiteral("values must be an array of strings");
        }
    }
    command.insert(QStringLiteral("values"), values);
    return QString();
}

// Tool-specific extras the generic ref/scalar-arg copiers do not cover. Returns an error
// string (empty on success).
QString applyToolExtras(const QString& tool,
                        const QJsonObject& arguments,
                        const QJsonObject& ref_index,
                        QJsonObject& command) {
    if (tool == QLatin1String("browser_drag")) {
        return applyDragExtra(arguments, ref_index, command);
    }
    if (tool == QLatin1String("browser_select")) {
        return applySelectValues(arguments, command);
    }
    return QString();
}

// The set of argument keys a tool legitimately accepts: its scalar arg specs, the ref/to_ref
// endpoints, and browser_select's `values`. Mirrors the additionalProperties:false the schema
// advertises so a caller cannot smuggle an unmodeled key past this layer.
QSet<QString> allowedArgKeys(const QString& tool, const CmdSpec& spec) {
    QSet<QString> allowed;
    if (spec.m_ref_mode != QLatin1String("none")) {
        allowed.insert(QStringLiteral("ref"));
    }
    for (const ArgSpec& arg : spec.m_args) {
        allowed.insert(arg.m_key);
    }
    if (tool == QLatin1String("browser_drag")) {
        allowed.insert(QStringLiteral("to_ref"));
    }
    if (tool == QLatin1String("browser_select")) {
        allowed.insert(QStringLiteral("values"));
    }
    return allowed;
}

// Reject an argument object carrying any key the tool does not model. Fails closed rather than
// ignoring the extra: an unknown key is a malformed call (typo, wrong tool, injection attempt),
// not something to silently drop.
QString rejectUnknownArgs(const QString& tool, const CmdSpec& spec, const QJsonObject& args) {
    const QSet<QString> allowed = allowedArgKeys(tool, spec);
    for (auto it = args.constBegin(); it != args.constEnd(); ++it) {
        if (!allowed.contains(it.key())) {
            return QStringLiteral("Unknown argument '%1' for %2").arg(it.key(), tool);
        }
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
                  QStringLiteral("Make the tab at the given index active. Call browser_tabs first: "
                                 "an index is only meaningful against a listing, and the call is "
                                 "refused if the tabs moved since that listing so the index cannot "
                                 "land on a tab you did not choose."),
                  toolSchema(QJsonObject{{QStringLiteral("index"),
                                          typedProperty(QStringLiteral("integer"),
                                                        QStringLiteral("Zero-based tab index from "
                                                                       "the last browser_tabs "
                                                                       "listing."))}},
                             QJsonArray{QStringLiteral("index")})));
    tools.append(toolEntry(
        QStringLiteral("browser_new_tab"),
        QStringLiteral("Open a new tab, optionally navigating to a URL."),
        toolSchema(QJsonObject{{QStringLiteral("url"),
                                stringProperty(QStringLiteral("URL to open (optional)."))}},
                   {})));
    tools.append(toolEntry(
        QStringLiteral("browser_close_tab"),
        QStringLiteral("Close the tab at index, or the active tab if omitted. When you pass an "
                       "index, call browser_tabs first: closing is not undoable, so the index is "
                       "checked against that listing and refused if the tabs moved."),
        toolSchema(QJsonObject{{QStringLiteral("index"),
                                typedProperty(QStringLiteral("integer"),
                                              QStringLiteral("Zero-based tab index from the last "
                                                             "browser_tabs listing (optional; "
                                                             "omit for the active tab)."))}},
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

void appendPermissionTool(QJsonArray& tools) {
    tools.append(toolEntry(
        QStringLiteral("browser_permission"),
        QStringLiteral(
            "Set a site permission for an origin so automation can pre-answer prompts: "
            "name is one of geolocation, notifications, camera, microphone, images, "
            "javascript, popups, automatic_downloads; setting is allow, block, or ask "
            "(the default). setting=allow REQUIRES an explicit origin (e.g. "
            "\"https://site.com\"): a grant is persistent and origin-scoped, so the site "
            "being handed the camera has to be named in the request rather than left as "
            "whatever tab happens to be in front. block and ask may omit it and apply to "
            "the active tab."),
        toolSchema(
            QJsonObject{
                {QStringLiteral("name"),
                 stringProperty(QStringLiteral("Permission to set, e.g. geolocation or camera."))},
                {QStringLiteral("setting"),
                 stringProperty(QStringLiteral("allow, block, or ask (default)."))},
                {QStringLiteral("origin"),
                 stringProperty(QStringLiteral("Origin the setting applies to. Required for "
                                               "setting=allow; defaults to the active tab for "
                                               "block and ask."))}},
            QJsonArray{QStringLiteral("name"), QStringLiteral("setting")})));
}

void appendStorageTool(QJsonArray& tools) {
    tools.append(toolEntry(
        QStringLiteral("browser_storage"),
        QStringLiteral(
            "Read or write the active tab's web storage: action is get, set, remove, "
            "clear, or keys; area is local (default) or session. get/set/remove need a "
            "key; set needs a string value. Returns the value, the key list, or a count. "
            "Values/keys are capped to keep results bounded."),
        toolSchema(
            QJsonObject{{QStringLiteral("action"),
                         stringProperty(QStringLiteral("get, set, remove, clear, or keys."))},
                        {QStringLiteral("area"),
                         stringProperty(QStringLiteral("local (default) or session."))},
                        {QStringLiteral("key"),
                         stringProperty(QStringLiteral("Storage key for get/set/remove."))},
                        {QStringLiteral("value"),
                         stringProperty(QStringLiteral("String value to store for set."))},
                        {QStringLiteral("expect_origin"),
                         stringProperty(QStringLiteral(
                             "Origin you expect the active tab to be on, e.g. "
                             "https://example.com (optional). The call is refused if the "
                             "tab is somewhere else, so a tab change cannot redirect the "
                             "read or write at another site's storage."))}},
            QJsonArray{QStringLiteral("action")})));
}

void appendCookiesTool(QJsonArray& tools) {
    tools.append(toolEntry(
        QStringLiteral("browser_cookies"),
        QStringLiteral(
            "Read, set, or remove cookies for a url (defaults to the active tab). "
            "action=get returns the cookies including their values (may include session "
            "tokens); set needs name+value (options: path, secure, http_only, same_site, "
            "expires_days -- a set is confined to the url's domain); remove needs name."),
        toolSchema(
            QJsonObject{
                {QStringLiteral("action"), stringProperty(QStringLiteral("get, set, or remove."))},
                {QStringLiteral("url"),
                 stringProperty(
                     QStringLiteral("Cookie url (optional; defaults to the active tab)."))},
                {QStringLiteral("name"),
                 stringProperty(QStringLiteral("Cookie name for set/remove."))},
                {QStringLiteral("value"), stringProperty(QStringLiteral("Cookie value for set."))},
                {QStringLiteral("path"),
                 stringProperty(QStringLiteral("Cookie path for set (optional)."))},
                {QStringLiteral("secure"),
                 typedProperty(QStringLiteral("boolean"),
                               QStringLiteral("Secure flag (optional)."))},
                {QStringLiteral("http_only"),
                 typedProperty(QStringLiteral("boolean"),
                               QStringLiteral("HttpOnly flag (optional)."))},
                {QStringLiteral("same_site"),
                 stringProperty(QStringLiteral("no_restriction, lax, or strict (optional)."))},
                {QStringLiteral("expires_days"),
                 typedProperty(QStringLiteral("number"),
                               QStringLiteral("Lifetime in days; omit for a session cookie."))}},
            QJsonArray{QStringLiteral("action")})));
}

void appendDownloadTool(QJsonArray& tools) {
    tools.append(toolEntry(
        QStringLiteral("browser_download"),
        QStringLiteral("Download a url to disk and wait for it to finish, returning the saved path "
                       "and byte size. filename is optional and must be a relative name (no "
                       "absolute path, no \"..\"); omit it to derive the name from the url. "
                       "timeout_ms bounds the wait (default 30000)."),
        toolSchema(
            QJsonObject{
                {QStringLiteral("url"), stringProperty(QStringLiteral("http(s) url to download."))},
                {QStringLiteral("filename"),
                 stringProperty(QStringLiteral("Relative save name, e.g. \"reports/a.pdf\" "
                                               "(optional)."))},
                {QStringLiteral("timeout_ms"),
                 typedProperty(QStringLiteral("integer"),
                               QStringLiteral("Max wait in ms (optional, default 30000)."))}},
            QJsonArray{QStringLiteral("url")})));
}

void appendHttpAuthTool(QJsonArray& tools) {
    tools.append(toolEntry(
        QStringLiteral("browser_http_auth"),
        QStringLiteral("Arm HTTP Basic/Digest credentials so 401 auth challenges on the active tab "
                       "are answered automatically (no native auth dialog). Give username + "
                       "password before navigating to a protected page; pass clear:true to disarm. "
                       "Credentials clear on a tab switch. The password is never echoed back."),
        toolSchema(
            QJsonObject{
                {QStringLiteral("username"),
                 stringProperty(QStringLiteral("Auth username (required unless clear)."))},
                {QStringLiteral("password"),
                 stringProperty(QStringLiteral("Auth password (used only to answer challenges)."))},
                {QStringLiteral("clear"),
                 typedProperty(QStringLiteral("boolean"),
                               QStringLiteral("Disarm and stop intercepting auth."))},
                {QStringLiteral("origin"),
                 stringProperty(QStringLiteral(
                     "Origin the credentials are for, e.g. https://example.com (optional "
                     "but recommended). Arming is refused if the active tab is somewhere "
                     "else, so a focus change cannot bind them to another site."))}},
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
//
// The list is cut twice -- once by the extension's own cap, once by this one -- and either cut
// has to be stated. The extension's cut arrives as omittedFramesTruncated rather than as a
// longer array, so the note does not depend on the two caps being kept numerically in step:
// raising one alone would otherwise turn a cut list back into an apparently complete one.
void appendOmittedFramesNote(const QJsonObject& capture, QString& outline) {
    const QJsonArray frames = capture.value(QStringLiteral("omittedFrames")).toArray();
    if (!frames.isEmpty()) {
        outline += QStringLiteral(
            "  ... (cross-origin iframe content not included; "
            "browser_navigate to a frame URL to read it):\n");
        bool cut = capture.value(QStringLiteral("omittedFramesTruncated")).toBool();
        int listed = 0;
        for (const QJsonValue& value : frames) {
            if (listed >= kMaxOmittedFramesListed) {
                cut = true;
                break;
            }
            const QString url = oneLine(value.toString(), kMaxUrlChars);
            if (url.isEmpty()) {
                continue;
            }
            outline += QStringLiteral("    - %1\n").arg(url);
            ++listed;
        }
        if (cut) {
            outline += QStringLiteral("    ... (more omitted frames)\n");
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
    int scanned = 0;
    for (const QJsonValue& value : nodes) {
        if (scanned >= kMaxScanNodes) {
            // A capture padded past the scan bound: stop and tell the model the outline is partial
            // rather than iterate an attacker-chosen node count.
            truncated = true;
            break;
        }
        ++scanned;
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
    appendPermissionTool(tools);
    appendStorageTool(tools);
    appendCookiesTool(tools);
    appendDownloadTool(tools);
    appendHttpAuthTool(tools);
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
    const QString unknown = rejectUnknownArgs(tool, spec, arguments);
    if (!unknown.isEmpty()) {
        return fail(unknown);
    }
    QJsonObject command{{QStringLiteral("cmd"), spec.m_cmd}};
    if (spec.m_ref_mode != QLatin1String("none")) {
        const QString error =
            resolveRef(arguments, ref_index, spec.m_ref_mode == QLatin1String("required"), command);
        if (!error.isEmpty()) {
            return fail(error);
        }
    }
    for (const ArgSpec& arg : spec.m_args) {
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
