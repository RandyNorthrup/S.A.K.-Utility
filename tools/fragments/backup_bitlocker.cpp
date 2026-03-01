void BackupBitlockerKeysAction::execute()
{
    if (isCancelled()) {
        emitCancelledResult("BitLocker key backup cancelled");
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();

    int total_keys_found = 0;
    int total_recovery_passwords = 0;
    if (!executeDiscoverVolumes(start_time)) return;
    if (!executeExtractKeys(start_time, total_keys_found, total_recovery_passwords)) return;

    QString backup_dir_path;
    int key_files_written = 0;
    bool permissions_set = false;
    if (!executeSaveKeyFiles(start_time, backup_dir_path, key_files_written, permissions_set)) return;

    executeBuildReport(start_time, total_keys_found, total_recovery_passwords,
                       backup_dir_path, key_files_written, permissions_set);
}

bool BackupBitlockerKeysAction::executeDiscoverVolumes(const QDateTime& start_time)
{
    Q_EMIT executionProgress("Detecting BitLocker volumes...", 5);

    if (m_volumes.isEmpty()) {
        m_volumes = detectEncryptedVolumes();
    }

    if (m_volumes.isEmpty()) {
        emitFailedResult("No BitLocker-encrypted volumes found",
                        "Ensure BitLocker is enabled on at least one volume and "
                        "the application is running with administrator privileges.",
                        start_time);
        return false;
    }

    if (isCancelled()) { emitCancelledResult("BitLocker key backup cancelled", start_time); return false; }
    return true;
}

bool BackupBitlockerKeysAction::executeExtractKeys(const QDateTime& start_time,
                                                    int& total_keys_found,
                                                    int& total_recovery_passwords)
{
    Q_EMIT executionProgress("Retrieving recovery keys...", 15);

    for (int i = 0; i < m_volumes.size(); ++i) {
        if (isCancelled()) { emitCancelledResult("BitLocker key backup cancelled", start_time); return false; }

        auto& vol = m_volumes[i];
        int progress = 15 + static_cast<int>((static_cast<double>(i) / m_volumes.size()) * 40);
        Q_EMIT executionProgress(
            QString("Retrieving keys for %1 (%2/%3)...")
                .arg(vol.drive_letter).arg(i + 1).arg(m_volumes.size()),
            progress);

        vol.key_protectors = getKeyProtectors(vol.drive_letter);
        total_keys_found += vol.key_protectors.size();

        for (const auto& kp : vol.key_protectors) {
            if (!kp.recovery_password.isEmpty()) {
                total_recovery_passwords++;
            }
        }

        // Log protector IDs only (never log the actual recovery passwords)
        for (const auto& kp : vol.key_protectors) {
            logInfo("  Volume {}: Key Protector {} ({})",
                   vol.drive_letter.toStdString(),
                   kp.protector_id.toStdString(),
                   kp.protector_type.toStdString());
        }
    }

    if (total_keys_found == 0) {
        emitFailedResult("No key protectors found on any volume",
                        "BitLocker volumes were detected but no key protectors could be read.\n"
                        "Ensure the application has administrator privileges.",
                        start_time);
        return false;
    }

    if (isCancelled()) { emitCancelledResult("BitLocker key backup cancelled", start_time); return false; }
    return true;
}

bool BackupBitlockerKeysAction::executeSaveKeyFiles(const QDateTime& start_time,
                                                     QString& backup_dir_path,
                                                     int& key_files_written,
                                                     bool& permissions_set)
{
    Q_EMIT executionProgress("Creating backup directory...", 60);

    QString timestamp = backupTimestamp();
    QString backup_dir_name = QString("BitLocker_Keys_%1").arg(timestamp);
    backup_dir_path = QDir(m_backup_location).filePath(backup_dir_name);

    QDir backup_dir(backup_dir_path);
    if (!backup_dir.mkpath(".")) {
        emitFailedResult("Failed to create backup directory",
                        "Could not create: " + backup_dir_path,
                        start_time);
        return false;
    }

    if (isCancelled()) { emitCancelledResult("BitLocker key backup cancelled", start_time); return false; }

    Q_EMIT executionProgress("Writing recovery key document...", 70);

    bool doc_written = writeRecoveryDocument(backup_dir_path);
    if (!doc_written) {
        emitFailedResult("Failed to write recovery key document", QString(), start_time);
        return false;
    }

    if (isCancelled()) { emitCancelledResult("BitLocker key backup cancelled", start_time); return false; }

    Q_EMIT executionProgress("Writing per-volume key files...", 80);

    key_files_written = writePerVolumeKeyFiles(backup_dir_path);

    if (isCancelled()) { emitCancelledResult("BitLocker key backup cancelled", start_time); return false; }

    Q_EMIT executionProgress("Writing JSON backup...", 85);

    if (!writeJsonBackup(backup_dir_path)) {
        emitFailedResult("Failed to write BitLocker key backup file", QString(), start_time);
        return false;
    }

    if (isCancelled()) { emitCancelledResult("BitLocker key backup cancelled", start_time); return false; }

    Q_EMIT executionProgress("Securing backup files...", 90);

    permissions_set = restrictFilePermissions(backup_dir_path);
    if (!permissions_set) {
        Q_EMIT logMessage("Warning: Could not restrict backup directory permissions");
    }

    return true;
}

bool BackupBitlockerKeysAction::writeJsonBackup(const QString& backup_dir_path)
{
    QJsonObject json_backup;
    json_backup["backup_version"] = "1.0";
    json_backup["created"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    json_backup["computer_name"] = QSysInfo::machineHostName();
    json_backup["os_version"] = QSysInfo::prettyProductName();

    QJsonArray volumes_json;
    for (const auto& vol : m_volumes) {
        QJsonObject vol_obj;
        vol_obj["drive_letter"] = vol.drive_letter;
        vol_obj["volume_label"] = vol.volume_label;
        vol_obj["device_id"] = vol.device_id;
        vol_obj["protection_status"] = vol.protection_status;
        vol_obj["encryption_method"] = vol.encryption_method;
        vol_obj["encryption_percentage"] = vol.encryption_percentage;
        vol_obj["lock_status"] = vol.lock_status;
        vol_obj["volume_type"] = vol.volume_type;
        vol_obj["volume_size_bytes"] = vol.volume_size_bytes;

        QJsonArray protectors_json;
        for (const auto& kp : vol.key_protectors) {
            QJsonObject kp_obj;
            kp_obj["protector_id"] = kp.protector_id;
            kp_obj["protector_type"] = kp.protector_type;
            if (!kp.recovery_password.isEmpty()) {
                kp_obj["recovery_password"] = kp.recovery_password;
            }
            if (!kp.key_file_name.isEmpty()) {
                kp_obj["key_file_name"] = kp.key_file_name;
            }
            protectors_json.append(kp_obj);
        }
        vol_obj["key_protectors"] = protectors_json;
        volumes_json.append(vol_obj);
    }
    json_backup["volumes"] = volumes_json;

    QString json_path = QDir(backup_dir_path).filePath("bitlocker_keys.json");
    QFile json_file(json_path);
    if (json_file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        json_file.write(QJsonDocument(json_backup).toJson(QJsonDocument::Indented));
        json_file.close();
        return true;
    }

    logError("Failed to write BitLocker key backup JSON: {}", json_file.errorString().toStdString());
    return false;
}

void BackupBitlockerKeysAction::executeBuildReport(const QDateTime& start_time,
                                                    int total_keys_found,
                                                    int total_recovery_passwords,
                                                    const QString& backup_dir_path,
                                                    int key_files_written,
                                                    bool permissions_set)
{
    Q_EMIT executionProgress("Finalizing backup...", 95);

    qint64 total_bytes = 0;
    int total_files = 0;
    QDirIterator dir_it(backup_dir_path, QDir::Files, QDirIterator::Subdirectories);
    while (dir_it.hasNext()) {
        dir_it.next();
        total_bytes += dir_it.fileInfo().size();
        total_files++;
    }

    Q_EMIT executionProgress("Backup complete", 100);

    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());

    ExecutionResult result;
    result.success = true;
    result.bytes_processed = total_bytes;
    result.files_processed = total_files;
    result.duration_ms = duration_ms;
    result.output_path = backup_dir_path;

    result.message = QString("Backed up %1 recovery key(s) from %2 volume(s)")
        .arg(total_recovery_passwords).arg(m_volumes.size());

    QStringList log_lines;
    log_lines.append("=== BitLocker Recovery Key Backup Summary ===");
    log_lines.append(QString("Computer: %1").arg(QSysInfo::machineHostName()));
    log_lines.append(QString("Date: %1").arg(QDateTime::currentDateTime().toString(Qt::ISODate)));
    log_lines.append(QString("Volumes: %1").arg(m_volumes.size()));
    log_lines.append(QString("Total key protectors: %1").arg(total_keys_found));
    log_lines.append(QString("Recovery passwords: %1").arg(total_recovery_passwords));
    log_lines.append(QString("Key files written: %1").arg(key_files_written));
    log_lines.append(QString("Backup location: %1").arg(backup_dir_path));
    log_lines.append(QString("Backup size: %1 bytes (%2 files)")
                    .arg(total_bytes).arg(total_files));
    if (permissions_set) {
        log_lines.append("File permissions: Restricted to current user + Administrators");
    }
    log_lines.append("");
    log_lines.append("IMPORTANT: Store this backup in a secure location.");
    log_lines.append("Recovery keys can unlock BitLocker-encrypted volumes.");
    result.log = log_lines.join("\n");

    finishWithResult(result, ActionStatus::Success);
}
