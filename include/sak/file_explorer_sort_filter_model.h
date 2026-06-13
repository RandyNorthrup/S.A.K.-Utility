// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_sort_filter_model.h
/// @brief Sort/filter proxy for File Explorer item rows.

#pragma once

#include <QSortFilterProxyModel>

namespace sak {

class FileExplorerSortFilterModel : public QSortFilterProxyModel {
    Q_OBJECT

public:
    explicit FileExplorerSortFilterModel(QObject* parent = nullptr);

    void setNameFilter(const QString& filter_text);
    void setShowHiddenItems(bool show);
    [[nodiscard]] QString nameFilter() const;
    [[nodiscard]] bool showHiddenItems() const;

protected:
    [[nodiscard]] bool lessThan(const QModelIndex& source_left,
                                const QModelIndex& source_right) const override;
    [[nodiscard]] bool filterAcceptsRow(int source_row,
                                        const QModelIndex& source_parent) const override;

private:
    QString m_name_filter;
    bool m_show_hidden_items{false};
};

}  // namespace sak
