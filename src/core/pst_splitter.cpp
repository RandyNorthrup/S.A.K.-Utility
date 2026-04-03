// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file pst_splitter.cpp
/// @brief PST volume splitting implementation

#include "sak/pst_splitter.h"

#include "sak/logger.h"
#include "sak/pst_writer.h"

#include <QFileInfo>

namespace sak {

// ============================================================================
// Construction / Destruction
// ============================================================================

PstSplitter::PstSplitter(const QString& base_path, qint64 max_size_bytes)
    : m_base_path(base_path), m_max_size(max_size_bytes) {
    Q_ASSERT(max_size_bytes > 0);
}

PstSplitter::~PstSplitter() = default;

// ============================================================================
// Public API
// ============================================================================

std::expected<void, error_code> PstSplitter::create() {
    m_volume_index = 1;
    m_current_writer = std::make_unique<PstWriter>(volumePath(m_volume_index));
    return m_current_writer->create();
}

std::expected<uint64_t, error_code> PstSplitter::createFolder(uint64_t parent_nid,
                                                              const QString& name,
                                                              const QString& container_class) {
    Q_ASSERT(m_current_writer);

    auto result = m_current_writer->createFolder(parent_nid, name, container_class);
    if (result.has_value()) {
        FolderInfo info;
        info.parent_nid = parent_nid;
        info.name = name;
        info.container_class = container_class;
        m_folder_info.insert(result.value(), info);
    }
    return result;
}

std::expected<void, error_code> PstSplitter::writeMessage(
    uint64_t folder_nid,
    const PstItemDetail& item,
    const QVector<QPair<QString, QByteArray>>& attachment_data) {
    Q_ASSERT(m_current_writer);

    // Check if we need to rotate
    if (m_current_writer->currentSize() >= m_max_size) {
        auto rotate_result = rotateVolume();
        if (!rotate_result.has_value()) {
            return std::unexpected(rotate_result.error());
        }
        // Map the folder NID to the new volume's NID
        if (m_folder_nid_map.contains(folder_nid)) {
            folder_nid = m_folder_nid_map.value(folder_nid);
        }
    }

    return m_current_writer->writeMessage(folder_nid, item, attachment_data);
}

std::expected<void, error_code> PstSplitter::finalizeAll() {
    if (m_current_writer) {
        m_total_bytes_written += m_current_writer->currentSize();
        auto result = m_current_writer->finalize();
        m_current_writer.reset();
        return result;
    }
    return {};
}

int PstSplitter::volumeCount() const {
    return m_volume_index;
}

qint64 PstSplitter::totalBytesWritten() const {
    qint64 total = m_total_bytes_written;
    if (m_current_writer) {
        total += m_current_writer->currentSize();
    }
    return total;
}

// ============================================================================
// Volume Rotation
// ============================================================================

std::expected<void, error_code> PstSplitter::rotateVolume() {
    // Finalize current volume
    if (m_current_writer) {
        m_total_bytes_written += m_current_writer->currentSize();
        auto fin = m_current_writer->finalize();
        if (!fin.has_value()) {
            return std::unexpected(fin.error());
        }
    }

    // Create next volume
    ++m_volume_index;
    m_current_writer = std::make_unique<PstWriter>(volumePath(m_volume_index));
    auto create_result = m_current_writer->create();
    if (!create_result.has_value()) {
        return std::unexpected(create_result.error());
    }

    logInfo("PstSplitter: rotated to volume {} ({})",
            std::to_string(m_volume_index),
            volumePath(m_volume_index).toStdString());

    // Recreate folder hierarchy in the new volume
    m_folder_nid_map.clear();
    for (auto it = m_folder_info.constBegin(); it != m_folder_info.constEnd(); ++it) {
        const auto& info = it.value();
        uint64_t old_nid = it.key();
        uint64_t parent = info.parent_nid;
        if (m_folder_nid_map.contains(parent)) {
            parent = m_folder_nid_map.value(parent);
        }
        auto new_nid = m_current_writer->createFolder(parent, info.name, info.container_class);
        if (new_nid.has_value()) {
            m_folder_nid_map.insert(old_nid, new_nid.value());
        }
    }

    return {};
}

QString PstSplitter::volumePath(int index) const {
    QFileInfo fi(m_base_path);
    return fi.absolutePath() + QStringLiteral("/") + fi.completeBaseName() +
           QStringLiteral("_part%1.pst").arg(index, 2, 10, QChar('0'));
}

}  // namespace sak
