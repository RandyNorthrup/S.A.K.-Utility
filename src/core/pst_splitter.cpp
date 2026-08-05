// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file pst_splitter.cpp
/// @brief PST volume splitting implementation

#include "sak/pst_splitter.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/pst_writer.h"

#include <QFileInfo>

namespace sak {

namespace {
constexpr int kPstPartNumberFieldWidth = 2;
}

// ============================================================================
// Construction / Destruction
// ============================================================================

PstSplitter::PstSplitter(const QString& base_path, qint64 max_size_bytes)
    : m_base_path(base_path), m_max_size(max_size_bytes) {
    // max_size_bytes crosses a public API boundary and this constructor has no error
    // channel, so the assert cannot be turned into a rejection here without changing the
    // type's construction contract. Guard it where there IS a channel instead: create()
    // refuses a non-positive volume size, so no volume is ever opened with one.
    Q_UNUSED(max_size_bytes)
}

PstSplitter::~PstSplitter() = default;

// ============================================================================
// Public API
// ============================================================================

std::expected<void, error_code> PstSplitter::create() {
    // The volume size arrives through the constructor, which has no way to reject it.
    // This is the first point with an error channel, so refuse here: a non-positive
    // maximum would make every writeMessage believe the volume is already over budget
    // and rotate on each message, producing an unbounded run of empty volumes.
    if (m_max_size <= 0) {
        logError("PstSplitter: refusing a non-positive volume size ({} bytes) for {}",
                 m_max_size,
                 m_base_path.toStdString());
        return std::unexpected(error_code::invalid_argument);
    }
    m_volume_index = 1;
    m_current_writer = std::make_unique<PstWriter>(volumePath(m_volume_index));
    return m_current_writer->create();
}

std::expected<uint64_t, error_code> PstSplitter::createFolder(uint64_t parent_nid,
                                                              const QString& name,
                                                              const QString& container_class) {
    // No current volume means create() never succeeded or finalizeAll() already
    // ran. Refuse instead of dereferencing a null writer.
    if (!m_current_writer) {
        logError("PstSplitter: createFolder called with no open volume ({})",
                 m_base_path.toStdString());
        return std::unexpected(error_code::write_error);
    }

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
    // Same volume-lifetime contract as createFolder().
    if (!m_current_writer) {
        logError("PstSplitter: writeMessage called with no open volume ({})",
                 m_base_path.toStdString());
        return std::unexpected(error_code::write_error);
    }

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

    // Recreate the folder hierarchy in the new volume parent-before-child. The
    // m_folder_info QHash has arbitrary iteration order, so recreating a child
    // before its parent would attach it to the stale volume-1 parent NID (which,
    // with m_next_nid restarting per volume, can collide with an unrelated new
    // folder and misattach messages). A parent that is not itself a tracked
    // folder is the fixed root and passes through unchanged.
    m_folder_nid_map.clear();
    QList<uint64_t> pending = m_folder_info.keys();
    while (!pending.isEmpty()) {
        bool progressed = false;
        for (int i = pending.size() - 1; i >= 0; --i) {
            const uint64_t old_nid = pending.at(i);
            const auto& info = m_folder_info.value(old_nid);
            uint64_t parent = info.parent_nid;
            if (m_folder_info.contains(parent)) {
                if (!m_folder_nid_map.contains(parent)) {
                    continue;  // parent folder not recreated yet; revisit next pass
                }
                parent = m_folder_nid_map.value(parent);
            }
            auto new_nid = m_current_writer->createFolder(parent, info.name, info.container_class);
            if (!new_nid.has_value()) {
                return std::unexpected(new_nid.error());
            }
            m_folder_nid_map.insert(old_nid, new_nid.value());
            pending.removeAt(i);
            progressed = true;
        }
        if (!progressed) {
            // Remaining folders have an unresolvable parent (cycle/orphan): fail the
            // rotation rather than silently misattaching them.
            return std::unexpected(error_code::write_error);
        }
    }

    return {};
}

QString PstSplitter::volumePath(int index) const {
    QFileInfo fi(m_base_path);
    return fi.absolutePath() + QStringLiteral("/") + fi.completeBaseName() +
           QStringLiteral("_part%1.pst")
               .arg(index, kPstPartNumberFieldWidth, kDecimalBase, QChar('0'));
}

}  // namespace sak
