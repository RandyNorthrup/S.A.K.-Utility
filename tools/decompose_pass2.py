"""Second-pass decomposition: split 4 methods still over 70 lines."""
import re, sys, os

def find_method_range(lines, class_name, method_name):
    """Find method start line and matching closing brace, aware of string literals."""
    sig = f'{class_name}::{method_name}('
    start = None
    for i, line in enumerate(lines):
        if sig in line:
            start = i
            break
    if start is None:
        return None, None

    depth = 0
    found_brace = False
    in_string = False
    for i in range(start, len(lines)):
        line = lines[i]
        j = 0
        while j < len(line):
            ch = line[j]
            if ch == '\\' and j + 1 < len(line):
                j += 2
                continue
            if ch == '"':
                in_string = not in_string
                j += 1
                continue
            if not in_string:
                if ch == '{':
                    depth += 1
                    found_brace = True
                elif ch == '}':
                    depth -= 1
                    if found_brace and depth == 0:
                        return start, i
            j += 1
    return start, None


def replace_method(filepath, class_name, method_name, new_code):
    """Replace an entire method body with new code."""
    with open(filepath, 'r', encoding='utf-8') as f:
        lines = f.readlines()

    start, end = find_method_range(lines, class_name, method_name)
    if start is None or end is None:
        print(f'  ERROR: Could not find {class_name}::{method_name} in {filepath}')
        return False

    count = end - start + 1
    print(f'  Found {class_name}::{method_name} at lines {start+1}-{end+1} ({count} lines)')

    new_lines = [l + '\n' if not l.endswith('\n') else l for l in new_code.split('\n')]
    lines[start:end+1] = new_lines

    with open(filepath, 'w', encoding='utf-8') as f:
        f.writelines(lines)

    print(f'  Replaced with {len(new_lines)} lines')
    return True


def insert_after_method(filepath, class_name, method_name, new_code):
    """Insert new code after the closing brace of a method."""
    with open(filepath, 'r', encoding='utf-8') as f:
        lines = f.readlines()

    start, end = find_method_range(lines, class_name, method_name)
    if start is None or end is None:
        print(f'  ERROR: Could not find {class_name}::{method_name} in {filepath}')
        return False

    new_lines = [l + '\n' if not l.endswith('\n') else l for l in new_code.split('\n')]
    # Insert after the closing brace line
    lines[end+1:end+1] = new_lines

    with open(filepath, 'w', encoding='utf-8') as f:
        f.writelines(lines)

    print(f'  Inserted {len(new_lines)} lines after {class_name}::{method_name}')
    return True


# ============================================================
# 1. clear_event_logs_action.cpp - executeBuildReport (79 -> ~30)
# ============================================================
def patch_clear_event_logs():
    filepath = r'c:\Users\Randy\Coding\S.A.K.-Utility\src\actions\clear_event_logs_action.cpp'
    print(f'\n=== Patching {os.path.basename(filepath)} ===')

    # Replace executeBuildReport with smaller orchestrator
    new_build_report = r'''void ClearEventLogsAction::executeBuildReport(const QDateTime& start_time,
                                               int total_logs, int cleared_logs,
                                               int total_entries, int backed_up,
                                               const QString& backup_path,
                                               const QStringList& details)
{
    Q_EMIT executionProgress("\u2560\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2563", 90);

    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());

    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.files_processed = cleared_logs;
    result.output_path = backup_path;

    QString log_output = "\u2554\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2557\n";
    log_output += "\u2551        EVENT LOG CLEARING - RESULTS                           \u2551\n";
    log_output += "\u2560\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2563\n";

    if (cleared_logs > 0) {
        result.success = true;
        result.message = QString("Successfully cleared %1 event log(s)").arg(cleared_logs);
        appendSuccessReport(log_output, total_logs, cleared_logs, total_entries,
                            backed_up, backup_path, details, duration_ms);
        result.log = log_output;
        finishWithResult(result, ActionStatus::Success);
    } else {
        result.success = false;
        result.message = "No event logs were cleared";
        appendFailureReport(log_output, details);
        result.log = log_output;
        finishWithResult(result, ActionStatus::Failed);
    }
}'''

    if not replace_method(filepath, 'ClearEventLogsAction', 'executeBuildReport', new_build_report):
        return False

    # Insert appendSuccessReport and appendFailureReport after executeBuildReport
    new_helpers = r'''
void ClearEventLogsAction::appendSuccessReport(QString& log_output,
                                                int total_logs, int cleared_logs,
                                                int total_entries, int backed_up,
                                                const QString& backup_path,
                                                const QStringList& details,
                                                qint64 duration_ms)
{
    log_output += QString("\u2551 Logs Processed: %1/%2\n").arg(cleared_logs).arg(total_logs).leftJustified(66) + "\u2551\n";
    log_output += QString("\u2551 Total Entries Cleared: %1\n").arg(total_entries).leftJustified(66) + "\u2551\n";
    log_output += QString("\u2551 Logs Backed Up: %1\n").arg(backed_up).leftJustified(66) + "\u2551\n";

    if (!backup_path.isEmpty()) {
        log_output += QString("\u2551 Backup Location: %1\n").arg(backup_path).leftJustified(66) + "\u2551\n";
    }

    log_output += "\u2560\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2563\n";
    log_output += QString("\u2551 CLEARED LOGS:                                                  \u2551\n");

    // Show first 10 cleared logs
    int shown = 0;
    for (const QString& detail : details) {
        if (detail.contains("Cleared") && shown < 10) {
            log_output += QString("\u2551 \u2022 %1\n").arg(detail).leftJustified(66) + "\u2551\n";
            shown++;
        }
    }

    if (details.size() > 10) {
        log_output += QString("\u2551 ... and %1 more\n").arg(details.size() - 10).leftJustified(66) + "\u2551\n";
    }

    log_output += "\u2560\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2563\n";
    log_output += QString("\u2551 Completed in: %1 seconds\n").arg(duration_ms / 1000.0, 0, 'f', 2).leftJustified(66) + "\u2551\n";
    log_output += "\u255a\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u255d\n";
}

void ClearEventLogsAction::appendFailureReport(QString& log_output,
                                                const QStringList& details)
{
    log_output += QString("\u2551 Status: No logs processed                                      \u2551\n");
    log_output += "\u2560\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2563\n";
    log_output += QString("\u2551 Reason: Administrator privileges may be required              \u2551\n");
    log_output += QString("\u2551 or all event logs are already empty                           \u2551\n");
    log_output += "\u2560\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2563\n";

    if (details.size() > 0) {
        log_output += QString("\u2551 ERROR DETAILS:                                                 \u2551\n");
        for (int i = 0; i < qMin(5, details.size()); i++) {
            log_output += QString("\u2551 %1\n").arg(details[i]).leftJustified(66) + "\u2551\n";
        }
    }

    log_output += "\u255a\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u255d\n";
}'''

    if not insert_after_method(filepath, 'ClearEventLogsAction', 'executeBuildReport', new_helpers):
        return False

    print('  clear_event_logs_action.cpp: OK')
    return True


# ============================================================
# 2. check_disk_errors_action.cpp - parseDriveScanResult (74 -> ~50)
# ============================================================
def patch_check_disk_errors():
    filepath = r'c:\Users\Randy\Coding\S.A.K.-Utility\src\actions\check_disk_errors_action.cpp'
    print(f'\n=== Patching {os.path.basename(filepath)} ===')

    new_parse = r'''void CheckDiskErrorsAction::parseDriveScanResult(const QString& output, QChar drive,
                                                   QString& report, int& drives_scanned,
                                                   int& errors_found, int& errors_fixed)
{
    Q_UNUSED(drive)
    QStringList lines = output.split('\n', Qt::SkipEmptyParts);

    bool parsing = false;
    QString drive_letter;
    QString status;
    bool has_corrupt = false;
    bool reboot_needed = false;
    bool scan_success = false;

    for (const QString& line : lines) {
        QString trimmed = line.trimmed();

        if (trimmed == "===SCAN_START===") {
            parsing = true;
            continue;
        }

        if (trimmed == "===SCAN_END===") {
            parsing = false;
            appendDriveScanEntry(drive_letter, status, has_corrupt, scan_success,
                                 reboot_needed, report, drives_scanned, errors_found);
            continue;
        }

        if (!parsing) continue;

        QStringList parts = trimmed.split(':', Qt::SkipEmptyParts);
        if (parts.size() < 2) continue;

        QString key = parts[0].trimmed();
        QString value = parts[1].trimmed();

        if (key == "Drive") {
            drive_letter = value;
        } else if (key == "OnlineScan" && value == "Success") {
            scan_success = true;
        } else if (key == "CorruptFile" && value == "Detected") {
            has_corrupt = true;
        } else if (key == "Status") {
            status = value;
        } else if (key == "RebootRequired" && value == "Yes") {
            reboot_needed = true;
        } else if (key == "OfflineRepair" && value == "Scheduled") {
            errors_fixed++;
        }
    }
}'''

    if not replace_method(filepath, 'CheckDiskErrorsAction', 'parseDriveScanResult', new_parse):
        return False

    new_helper = r'''
void CheckDiskErrorsAction::appendDriveScanEntry(const QString& drive_letter,
                                                   const QString& status,
                                                   bool has_corrupt,
                                                   bool scan_success,
                                                   bool reboot_needed,
                                                   QString& report,
                                                   int& drives_scanned,
                                                   int& errors_found)
{
    if (scan_success) {
        drives_scanned++;

        report += QString("Drive %1:\n").arg(drive_letter);
        report += QString("  Status: %1\n").arg(status);

        if (has_corrupt) {
            errors_found++;
            report += "  \u26a0 Corruption detected: $corrupt file found\n";
            report += "  \u2139 Offline repair scheduled at next reboot\n";
        } else {
            report += "  \u2713 No corruption detected\n";
        }

        if (reboot_needed) {
            report += "  \u26a0 REBOOT REQUIRED to complete repair\n";
        }
    } else {
        report += QString("Drive %1: - %2\n").arg(drive_letter).arg(status);
    }

    report += "\n";
}'''

    if not insert_after_method(filepath, 'CheckDiskErrorsAction', 'parseDriveScanResult', new_helper):
        return False

    print('  check_disk_errors_action.cpp: OK')
    return True


# ============================================================
# 3. check_bloatware_action.cpp - executeMatchBloatware (109 -> ~55 + ~60)
# ============================================================
def patch_check_bloatware():
    filepath = r'c:\Users\Randy\Coding\S.A.K.-Utility\src\actions\check_bloatware_action.cpp'
    print(f'\n=== Patching {os.path.basename(filepath)} ===')

    new_match = r'''void CheckBloatwareAction::executeMatchBloatware(const QString& scan_output,
                                                   QString& report,
                                                   QString& structured_output,
                                                   int& bloatware_count,
                                                   qint64& total_size,
                                                   int& apps_scanned)
{
    Q_EMIT executionProgress("Analyzing detected apps...", 40);

    // Comprehensive bloatware patterns with safety ratings
    const QMap<QString, QPair<QString, bool>> bloatware_patterns = buildBloatwarePatterns();

    // Parse JSON output
    QJsonDocument doc = QJsonDocument::fromJson(scan_output.toUtf8());
    QJsonArray apps = doc.isArray() ? doc.array() : QJsonArray();
    if (apps.isEmpty() && doc.isObject()) {
        apps.append(doc.object());  // Handle single object response
    }
    apps_scanned = apps.size();

    QVector<QPair<QString, QPair<QString, double>>> detected_bloatware;
    int installed_scanned = 0;
    int provisioned_scanned = 0;
    int safe_to_remove = 0;
    QSet<QString> seen;

    for (const QJsonValue& val : apps) {
        QJsonObject app = val.toObject();
        QString name = app["Name"].toString();
        QString package_full = app["PackageFullName"].toString();
        QString source = app["Source"].toString();
        double size_mb = app["SizeMB"].toDouble();

        const QString dedupe_key = package_full.isEmpty() ? name : package_full;
        if (dedupe_key.isEmpty() || seen.contains(dedupe_key)) {
            continue;
        }
        seen.insert(dedupe_key);

        if (source == "Provisioned") {
            provisioned_scanned++;
        } else {
            installed_scanned++;
        }

        // Check against patterns
        for (auto it = bloatware_patterns.begin(); it != bloatware_patterns.end(); ++it) {
            if (name.contains(it.key(), Qt::CaseInsensitive) || package_full.contains(it.key(), Qt::CaseInsensitive)) {
                bloatware_count++;
                detected_bloatware.append(qMakePair(name.isEmpty() ? package_full : name, qMakePair(it.value().first, size_mb)));
                total_size += static_cast<qint64>(size_mb * 1024 * 1024);
                if (it.value().second) safe_to_remove++;
                break;
            }
        }
    }

    formatBloatwareMatchReport(detected_bloatware, apps.size(), installed_scanned,
                                provisioned_scanned, safe_to_remove, bloatware_count,
                                report, structured_output);
}'''

    if not replace_method(filepath, 'CheckBloatwareAction', 'executeMatchBloatware', new_match):
        return False

    new_format = r'''
void CheckBloatwareAction::formatBloatwareMatchReport(
    const QVector<QPair<QString, QPair<QString, double>>>& detected,
    int apps_count, int installed_scanned, int provisioned_scanned,
    int safe_to_remove, int bloatware_count,
    QString& report, QString& structured_output)
{
    Q_EMIT executionProgress("Generating detailed report...", 70);

    // Phase 2: Report generation
    report += QString("\u2551 Apps Scanned: %1").arg(apps_count).leftJustified(73, ' ') + "\u2551\n";
    report += QString("\u2551 Installed (All Users): %1").arg(installed_scanned).leftJustified(73, ' ') + "\u2551\n";
    report += QString("\u2551 Provisioned (System Image): %1").arg(provisioned_scanned).leftJustified(73, ' ') + "\u2551\n";
    report += QString("\u2551 Bloatware Found: %1").arg(bloatware_count).leftJustified(73, ' ') + "\u2551\n";
    report += QString("\u2551 Safe to Remove: %1").arg(safe_to_remove).leftJustified(73, ' ') + "\u2551\n";
    qint64 total_size = 0;
    for (const auto& item : detected) {
        total_size += static_cast<qint64>(item.second.second * 1024 * 1024);
    }
    report += QString("\u2551 Total Size: %1 MB").arg(QString::number(total_size / (1024.0 * 1024.0), 'f', 2)).leftJustified(73, ' ') + "\u2551\n";
    report += "\u2560\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2563\n";

    if (bloatware_count > 0) {
        report += "\u2551 DETECTED BLOATWARE                                                   \u2551\n";
        report += "\u2560\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2563\n";

        int displayed = 0;
        for (const auto& item : detected) {
            if (displayed >= 20) break;  // Limit display
            QString name = item.first.left(40);
            QString category = item.second.first;
            double size_mb = item.second.second;

            report += QString("\u2551 \u2022 %1").arg(name).leftJustified(73, ' ') + "\u2551\n";
            report += QString("\u2551   Category: %1 | Size: %2 MB")
                         .arg(category).arg(QString::number(size_mb, 'f', 2)).leftJustified(73, ' ') + "\u2551\n";
            displayed++;
        }

        if (detected.size() > 20) {
            report += QString("\u2551   ... and %1 more app(s)").arg(detected.size() - 20).leftJustified(73, ' ') + "\u2551\n";
        }
    } else {
        report += "\u2551 \u2713 No common bloatware detected                                       \u2551\n";
    }

    // Structured output
    structured_output += QString("APPS_SCANNED:%1\n").arg(apps_count);
    structured_output += QString("BLOATWARE_FOUND:%1\n").arg(bloatware_count);
    structured_output += QString("SAFE_TO_REMOVE:%1\n").arg(safe_to_remove);
    structured_output += QString("INSTALLED_SCANNED:%1\n").arg(installed_scanned);
    structured_output += QString("PROVISIONED_SCANNED:%1\n").arg(provisioned_scanned);
    structured_output += QString("TOTAL_SIZE_MB:%1\n").arg(QString::number(total_size / (1024.0 * 1024.0), 'f', 2));
    structured_output += QString("SPACE_RECLAIMABLE_MB:%1\n").arg(QString::number(total_size / (1024.0 * 1024.0), 'f', 2));

    for (int i = 0; i < qMin(detected.size(), 20); ++i) {
        structured_output += QString("BLOATWARE_%1:%2|%3|%4MB\n")
            .arg(i + 1)
            .arg(detected[i].first)
            .arg(detected[i].second.first)
            .arg(QString::number(detected[i].second.second, 'f', 2));
    }
}'''

    if not insert_after_method(filepath, 'CheckBloatwareAction', 'executeMatchBloatware', new_format):
        return False

    print('  check_bloatware_action.cpp: OK')
    return True


# ============================================================
# 4. windows_update_action.cpp - executeInitSession (95 -> ~7 + 45 + 45)
# ============================================================
def patch_windows_update():
    filepath = r'c:\Users\Randy\Coding\S.A.K.-Utility\src\actions\windows_update_action.cpp'
    print(f'\n=== Patching {os.path.basename(filepath)} ===')

    new_init = r'''void WindowsUpdateAction::executeInitSession(const QDateTime& start_time, QString& ps_script)
{
    Q_UNUSED(start_time)
    Q_EMIT executionProgress("Initiating Windows Update scan...", 5);

    // Modern Windows 10/11 approach: Use UsoClient (Update Session Orchestrator)
    // Per Microsoft docs: UsoClient replaces deprecated COM API for Windows Update automation
    // Commands: StartScan, StartDownload, StartInstall, ResumeUpdate, RestartDevice

    ps_script = buildUpdateScanScript() + buildUpdateInstallScript();
}'''

    if not replace_method(filepath, 'WindowsUpdateAction', 'executeInitSession', new_init):
        return False

    new_helpers = r'''
QString WindowsUpdateAction::buildUpdateScanScript()
{
    return
        "# Enterprise Windows Update using UsoClient\n"
        "Write-Output 'Starting update scan via UsoClient...'; \n"
        "$usoClient = Join-Path $env:SystemRoot 'System32\\UsoClient.exe'; \n"
        "\n"
        "# Step 1: Scan for updates\n"
        "if (Test-Path $usoClient) { \n"
        "  try { \n"
        "    Start-Process -FilePath $usoClient -ArgumentList 'StartScan' -NoNewWindow -Wait; \n"
        "    Write-Output 'Scan initiated'; \n"
        "    Start-Sleep -Seconds 10; \n"
        "  } catch { \n"
        "    Write-Error \"Scan failed: $_\"; \n"
        "    exit 1; \n"
        "  } \n"
        "} else { \n"
        "  Write-Error 'UsoClient not found'; \n"
        "  exit 1; \n"
        "} \n"
        "\n"
        "# Check for available updates using Windows Update API\n"
        "Write-Output 'Checking update status...'; \n"
        "try { \n"
        "  $updateSession = New-Object -ComObject Microsoft.Update.Session; \n"
        "  $updateSearcher = $updateSession.CreateUpdateSearcher(); \n"
        "  $searchResult = $updateSearcher.Search('IsInstalled=0 and Type=\'Software\' and IsHidden=0'); \n"
        "  $updateCount = $searchResult.Updates.Count; \n"
        "  Write-Output \"Found $updateCount update(s)\"; \n"
        "  \n"
        "  if ($updateCount -eq 0) { \n"
        "    Write-Output 'No updates available'; \n"
        "    exit 0; \n"
        "  } \n"
        "  \n"
        "  # List update titles\n"
        "  foreach ($update in $searchResult.Updates) { \n"
        "    Write-Output \"  - $($update.Title)\"; \n"
        "  } \n"
        "} catch { \n"
        "  Write-Warning \"Could not query updates: $_\"; \n"
        "  # Continue anyway - UsoClient will handle\n"
        "} \n"
        "\n";
}

QString WindowsUpdateAction::buildUpdateInstallScript()
{
    return
        "# Step 2: Download updates\n"
        "Write-Output 'Starting download via UsoClient...'; \n"
        "try { \n"
        "  Start-Process -FilePath $usoClient -ArgumentList 'StartDownload' -NoNewWindow -Wait; \n"
        "  Write-Output 'Download initiated'; \n"
        "  Start-Sleep -Seconds 15; \n"
        "} catch { \n"
        "  Write-Error \"Download failed: $_\"; \n"
        "  exit 1; \n"
        "} \n"
        "\n"
        "# Step 3: Install updates\n"
        "Write-Output 'Starting installation via UsoClient...'; \n"
        "try { \n"
        "  Start-Process -FilePath $usoClient -ArgumentList 'StartInstall' -NoNewWindow -Wait; \n"
        "  Write-Output 'Installation initiated'; \n"
        "  Start-Sleep -Seconds 20; \n"
        "} catch { \n"
        "  Write-Error \"Installation failed: $_\"; \n"
        "  exit 1; \n"
        "} \n"
        "\n"
        "# Check if reboot required\n"
        "$rebootRequired = $false; \n"
        "try { \n"
        "  $systemInfo = New-Object -ComObject Microsoft.Update.SystemInfo; \n"
        "  $rebootRequired = $systemInfo.RebootRequired; \n"
        "} catch { \n"
        "  # Also check registry\n"
        "  $regPath = 'HKLM:\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update\\RebootRequired'; \n"
        "  if (Test-Path $regPath) { \n"
        "    $rebootRequired = $true; \n"
        "  } \n"
        "} \n"
        "\n"
        "if ($rebootRequired) { \n"
        "  Write-Output 'REBOOT_REQUIRED'; \n"
        "} else { \n"
        "  Write-Output 'Installation completed successfully'; \n"
        "} \n"
        "\n"
        "exit 0";
}'''

    if not insert_after_method(filepath, 'WindowsUpdateAction', 'executeInitSession', new_helpers):
        return False

    print('  windows_update_action.cpp: OK')
    return True


if __name__ == '__main__':
    ok = True
    ok = patch_clear_event_logs() and ok
    ok = patch_check_disk_errors() and ok
    ok = patch_check_bloatware() and ok
    ok = patch_windows_update() and ok

    if ok:
        print('\nAll 4 decompositions applied successfully!')
    else:
        print('\nERROR: Some patches failed')
        sys.exit(1)
