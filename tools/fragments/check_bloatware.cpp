void CheckBloatwareAction::execute() {
    if (isCancelled()) {
        emitCancelledResult("Bloatware check cancelled");
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();

    QString scan_output;
    QString report;
    executeScanApps(start_time, scan_output, report);

    QString structured_output;
    int bloatware_count = 0;
    qint64 total_size = 0;
    int apps_scanned = 0;
    executeMatchBloatware(scan_output, report, structured_output,
                          bloatware_count, total_size, apps_scanned);

    executeBuildReport(start_time, apps_scanned, bloatware_count,
                       total_size, report, structured_output);
}

void CheckBloatwareAction::executeScanApps(const QDateTime& start_time,
                                            QString& scan_output,
                                            QString& report)
{
    Q_EMIT executionProgress("Scanning for bloatware apps...", 10);

    // Phase 1: Scan UWP apps with Get-AppxPackage
    report += "╔══════════════════════════════════════════════════════════════════════╗\n";
    report += "║                      BLOATWARE ANALYSIS                              ║\n";
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    report += "║ Phase 1: UWP App Scan (All Users + Provisioned)                     ║\n";
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";

    ProcessResult ps_scan = runPowerShell(
        R"(
            $installed = Get-AppxPackage -AllUsers | Select-Object Name, PackageFullName, InstallLocation, @{N='SizeMB';E={
                if ($_.InstallLocation -and (Test-Path $_.InstallLocation)) {
                    [Math]::Round((Get-ChildItem $_.InstallLocation -Recurse -ErrorAction SilentlyContinue | Measure-Object -Property Length -Sum).Sum / 1MB, 2)
                } else { 0 }
            }}, IsBundle, Architecture, Version, PublisherId, @{N='Source';E={'Installed'}}
            $provisioned = Get-AppxProvisionedPackage -Online | Select-Object @{N='Name';E={$_.DisplayName}}, @{N='PackageFullName';E={$_.PackageName}}, @{N='InstallLocation';E={''}}, @{N='SizeMB';E={0}}, @{N='Source';E={'Provisioned'}}
            $all = @($installed + $provisioned) | Where-Object { $_.PackageFullName } | Sort-Object PackageFullName -Unique
            $all | ConvertTo-Json
        )",
        30000);
    if (!ps_scan.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Bloatware detail scan warning: " + ps_scan.std_err.trimmed());
    }
    scan_output = ps_scan.std_out.trimmed();
}

void CheckBloatwareAction::executeMatchBloatware(const QString& scan_output,
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

    Q_EMIT executionProgress("Generating detailed report...", 70);

    // Phase 2: Report generation
    report += QString("║ Apps Scanned: %1").arg(apps.size()).leftJustified(73, ' ') + "║\n";
    report += QString("║ Installed (All Users): %1").arg(installed_scanned).leftJustified(73, ' ') + "║\n";
    report += QString("║ Provisioned (System Image): %1").arg(provisioned_scanned).leftJustified(73, ' ') + "║\n";
    report += QString("║ Bloatware Found: %1").arg(bloatware_count).leftJustified(73, ' ') + "║\n";
    report += QString("║ Safe to Remove: %1").arg(safe_to_remove).leftJustified(73, ' ') + "║\n";
    report += QString("║ Total Size: %1 MB").arg(QString::number(total_size / (1024.0 * 1024.0), 'f', 2)).leftJustified(73, ' ') + "║\n";
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";

    if (bloatware_count > 0) {
        report += "║ DETECTED BLOATWARE                                                   ║\n";
        report += "╠══════════════════════════════════════════════════════════════════════╣\n";

        int displayed = 0;
        for (const auto& item : detected_bloatware) {
            if (displayed >= 20) break;  // Limit display
            QString name = item.first.left(40);
            QString category = item.second.first;
            double size_mb = item.second.second;

            report += QString("║ • %1").arg(name).leftJustified(73, ' ') + "║\n";
            report += QString("║   Category: %1 | Size: %2 MB")
                         .arg(category).arg(QString::number(size_mb, 'f', 2)).leftJustified(73, ' ') + "║\n";
            displayed++;
        }

        if (detected_bloatware.size() > 20) {
            report += QString("║   ... and %1 more app(s)").arg(detected_bloatware.size() - 20).leftJustified(73, ' ') + "║\n";
        }
    } else {
        report += "║ ✓ No common bloatware detected                                       ║\n";
    }

    // Structured output
    structured_output += QString("APPS_SCANNED:%1\n").arg(apps.size());
    structured_output += QString("BLOATWARE_FOUND:%1\n").arg(bloatware_count);
    structured_output += QString("SAFE_TO_REMOVE:%1\n").arg(safe_to_remove);
    structured_output += QString("INSTALLED_SCANNED:%1\n").arg(installed_scanned);
    structured_output += QString("PROVISIONED_SCANNED:%1\n").arg(provisioned_scanned);
    structured_output += QString("TOTAL_SIZE_MB:%1\n").arg(QString::number(total_size / (1024.0 * 1024.0), 'f', 2));
    structured_output += QString("SPACE_RECLAIMABLE_MB:%1\n").arg(QString::number(total_size / (1024.0 * 1024.0), 'f', 2));

    for (int i = 0; i < qMin(detected_bloatware.size(), 20); ++i) {
        structured_output += QString("BLOATWARE_%1:%2|%3|%4MB\n")
            .arg(i + 1)
            .arg(detected_bloatware[i].first)
            .arg(detected_bloatware[i].second.first)
            .arg(QString::number(detected_bloatware[i].second.second, 'f', 2));
    }
}

void CheckBloatwareAction::executeBuildReport(const QDateTime& start_time, int apps_scanned,
                                               int bloatware_count, qint64 total_size,
                                               QString& report,
                                               const QString& structured_output)
{
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    report += "║ REMOVAL INFORMATION                                                  ║\n";
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    report += "║ To remove bloatware apps, use PowerShell:                            ║\n";
    report += "║                                                                      ║\n";
    report += "║ Remove for current user:                                             ║\n";
    report += "║   Get-AppxPackage *AppName* | Remove-AppxPackage                     ║\n";
    report += "║                                                                      ║\n";
    report += "║ Remove for all users (requires admin):                               ║\n";
    report += "║   Get-AppxPackage *AppName* -AllUsers | Remove-AppxPackage -AllUsers ║\n";
    report += "║                                                                      ║\n";
    report += "║ Remove provisioning (prevents reinstall):                            ║\n";
    report += "║   Get-AppxProvisionedPackage -Online | Where {$_.DisplayName -match  ║\n";
    report += "║   \"AppName\"} | Remove-AppxProvisionedPackage -Online                 ║\n";
    report += "║                                                                      ║\n";
    report += "║ ⚠ CAUTION: Some apps may be needed by certain users                  ║\n";
    report += "║   Always verify before removing system applications                  ║\n";
    report += "╚══════════════════════════════════════════════════════════════════════╝\n";

    Q_EMIT executionProgress("Analysis complete", 100);

    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());

    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.files_processed = apps_scanned;
    result.success = true;
    result.message = bloatware_count > 0
        ? QString("Found %1 bloatware app(s) using %2 MB").arg(bloatware_count).arg(QString::number(total_size / (1024.0 * 1024.0), 'f', 2))
        : "No common bloatware detected";
    result.log = report + "\n" + structured_output;

    finishWithResult(result, ActionStatus::Success);
}
