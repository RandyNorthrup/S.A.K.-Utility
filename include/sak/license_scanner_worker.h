// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/worker_base.h"
#include <QString>
#include <QVector>
#include <QSet>
#include <expected>

namespace sak {

class license_scanner_worker : public WorkerBase {
    Q_OBJECT

public:
    struct license_info {
        QString product_name;
        QString license_key;
        QString registry_path;
        QString installation_path;
        QString version;
        bool is_valid;
    };

    struct config {
        bool scan_registry;
        bool scan_filesystem;
        QVector<QString> additional_paths;
        bool include_system_licenses;
        bool validate_keys;
    };

    explicit license_scanner_worker(const config& cfg, QObject* parent = nullptr);
    ~license_scanner_worker() override = default;

Q_SIGNALS:
    void scan_progress(int current, int total, const QString& location);
    void license_found(const QString& product_name, const QString& license_key);
    void scan_complete(int total_count);

protected:
    auto execute() -> std::expected<void, sak::error_code> override;

private:
    std::expected<QVector<license_info>, sak::error_code> scan_registry();
    std::expected<QVector<license_info>, sak::error_code> scan_filesystem();
    std::expected<QVector<license_info>, sak::error_code> scan_common_locations();
    
    bool is_valid_license_key(const QString& key) const;
    QString normalize_license_key(const QString& key) const;
    bool check_and_mark_duplicate(const license_info& info);
    
    config m_config;
    QVector<license_info> m_found_licenses;
    QSet<QString> m_processed_keys;
};

} // namespace sak
