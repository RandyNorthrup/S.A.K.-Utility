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
           role == QLatin1String("presentation") || role == QLatin1String("InlineTextBox");
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

QString stateSuffix(const QJsonObject& node) {
    QStringList flags;
    if (node.value(QStringLiteral("editable")).toBool()) {
        flags << QStringLiteral("editable");
    }
    if (node.value(QStringLiteral("disabled")).toBool()) {
        flags << QStringLiteral("disabled");
    }
    if (node.contains(QStringLiteral("checked"))) {
        flags << (node.value(QStringLiteral("checked")).toBool() ? QStringLiteral("checked")
                                                                 : QStringLiteral("unchecked"));
    }
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
    QString type;  // "string" | "int" | "bool"
    bool required{false};
};

struct CmdSpec {
    QString cmd;
    QString ref_mode;  // "none" | "optional" | "required"
    QVector<ArgSpec> args;
};

const QHash<QString, CmdSpec>& commandSpecs() {
    static const QHash<QString, CmdSpec> specs = {
        {QStringLiteral("browser_navigate"),
         {QStringLiteral("navigate"),
          QStringLiteral("none"),
          {{QStringLiteral("url"), QStringLiteral("string"), true}}}},
        {QStringLiteral("browser_snapshot"),
         {QStringLiteral("snapshot"), QStringLiteral("none"), {}}},
        {QStringLiteral("browser_click"),
         {QStringLiteral("click"), QStringLiteral("required"), {}}},
        {QStringLiteral("browser_type"),
         {QStringLiteral("type"),
          QStringLiteral("optional"),
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
    };
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
        QStringLiteral("Click the element with the given [ref] from the latest snapshot. The "
                       "click is injected at the browser level, so the user's mouse is untouched."),
        toolSchema(QJsonObject{{QStringLiteral("ref"),
                                stringProperty(QStringLiteral("Element ref, e.g. \"e5\"."))}},
                   QJsonArray{QStringLiteral("ref")})));
    tools.append(toolEntry(
        QStringLiteral("browser_type"),
        QStringLiteral("Type text into the element with [ref] (or the focused element if omitted). "
                       "Set submit to press Enter afterward."),
        toolSchema(QJsonObject{{QStringLiteral("text"),
                                stringProperty(QStringLiteral("Text to type."))},
                               {QStringLiteral("ref"),
                                stringProperty(QStringLiteral("Target element ref (optional)."))},
                               {QStringLiteral("submit"),
                                typedProperty(QStringLiteral("boolean"),
                                              QStringLiteral("Press Enter after typing."))}},
                   QJsonArray{QStringLiteral("text")})));
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
    bool truncated = false;
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
        // Only an interactable node with a valid integer backendNodeId gets a ref;
        // otherwise it is shown for context but cannot be acted on.
        qint64 backend_id = 0;
        QString ref;
        if (interactable && asBackendId(node.value(QStringLiteral("backendNodeId")), &backend_id)) {
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

    view.outline = outline;
    view.ref_index = ref_index;
    view.element_count = next_ref;
    return view;
}

QJsonArray browserToolCatalog() {
    QJsonArray tools;
    appendNavTools(tools);
    appendActionTools(tools);
    appendTabTools(tools);
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
    return {command, true, QString()};
}

}  // namespace sak::win32mcp::browser
