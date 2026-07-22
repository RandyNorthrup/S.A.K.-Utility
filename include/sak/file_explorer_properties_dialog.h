// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_properties_dialog.h
/// @brief Files-style Properties window for the File Management Explorer.

#pragma once

#include "sak/file_explorer_properties_calc.h"
#include "sak/file_management_file_system.h"

#include <QDialog>
#include <QFutureWatcher>
#include <QMap>

class QCheckBox;
class QLabel;
class QLineEdit;
class QTabWidget;

namespace sak {

/// The Files Properties window (OpenPropertiesAction, Alt+Enter): a General
/// page in the GeneralPage.xaml field order and, for a single file, a Hashes
/// page whose digests compute lazily when the tab first opens. All data flows
/// through the bridge, so raw APFS/HFS entries behave exactly like mounted
/// ones. Multi-selections show the combined General page only.
class FileExplorerPropertiesDialog : public QDialog {
    Q_OBJECT

public:
    FileExplorerPropertiesDialog(FileManagementTarget target,
                                 QVector<FileManagementEntry> entries,
                                 QWidget* parent = nullptr);
    ~FileExplorerPropertiesDialog() override;

    /// The (possibly edited) name field text; the panel commits a rename when
    /// it differs from the original single entry's name after accept.
    [[nodiscard]] QString editedName() const;
    [[nodiscard]] QString originalName() const;

private:
    void buildGeneralPage();
    void buildHashesPage();
    void startSizeCalculation();
    void startHashCalculation();
    void applyHashes(const QMap<QString, QString>& hashes);

    FileManagementTarget m_target;
    QVector<FileManagementEntry> m_entries;
    QTabWidget* m_tabs{nullptr};
    QLineEdit* m_name_edit{nullptr};
    QLabel* m_size_label{nullptr};
    QMap<QString, QCheckBox*> m_hash_checks;
    QMap<QString, QLabel*> m_hash_values;
    bool m_hashes_started{false};
    QFutureWatcher<sak::TreeSizeResult> m_size_watcher;
    QFutureWatcher<QMap<QString, QString>> m_hash_watcher;
};

}  // namespace sak
