// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_status_center.h"

#include "sak/format_utils.h"

#include <QDateTime>

#include <algorithm>

namespace sak {

namespace {

// Files IntervalSampler default (StatusCenterItemProgressModel ctor).
constexpr qint64 kSamplerIntervalMs = 100;
// Files SpeedGraph point spacing guard (StatusCenterItem.ReportProgress).
constexpr double kGraphMinPercentStep = 0.5;

/// Card header templates per operation, indexed by result branch
/// {InProgress, Success, Failed, Cancelled} (Files Strings/en-US/Resources.resw
/// StatusCenter_* entries; Recycle reuses the Delete headers per
/// StatusCenterHelper.AddCard_Recycle).
struct CardPatterns {
    const char* branches[4];
};

const CardPatterns& patternsFor(const FileExplorerOperationType operation) {
    static const CardPatterns copy_patterns{{"Copying %1 to \"%2\"",
                                             "Copied %1 to \"%2\"",
                                             "Error copying %1 to \"%2\"",
                                             "Canceled copying %1 to \"%2\""}};
    static const CardPatterns move_patterns{{"Moving %1 to \"%2\"",
                                             "Moved %1 to \"%2\"",
                                             "Error moving %1 to \"%2\"",
                                             "Canceled moving %1 to \"%2\""}};
    static const CardPatterns delete_patterns{{"Deleting %1 from \"%2\"",
                                               "Deleted %1 from \"%2\"",
                                               "Error deleting %1 from \"%2\"",
                                               "Canceled deleting %1 from \"%2\""}};
    static const CardPatterns compress_patterns{{"Compressing %1 to \"%2\"",
                                                 "Compressed %1 to \"%2\"",
                                                 "Error compressing %1 to \"%2\"",
                                                 "Canceled compressing %1 to \"%2\""}};
    static const CardPatterns extract_patterns{{"Extracting \"%1\" to \"%2\"",
                                                "Extracted \"%1\" to \"%2\"",
                                                "Error extracting \"%1\" to \"%2\"",
                                                "Canceled extracting \"%1\" to \"%2\""}};
    static const CardPatterns prepare_patterns{{"Preparing the operation...",
                                                "Preparing the operation...",
                                                "Preparing the operation...",
                                                "Preparing the operation..."}};
    switch (operation) {
    case FileExplorerOperationType::Move:
        return move_patterns;
    case FileExplorerOperationType::Delete:
    case FileExplorerOperationType::Recycle:
        return delete_patterns;
    case FileExplorerOperationType::Compressed:
        return compress_patterns;
    case FileExplorerOperationType::Extract:
        return extract_patterns;
    case FileExplorerOperationType::Prepare:
        return prepare_patterns;
    case FileExplorerOperationType::Copy:
    default:
        return copy_patterns;
    }
}

int branchForResult(const FileExplorerReturnResult result) {
    if (result == FileExplorerReturnResult::InProgress) {
        return 0;
    }
    if (result == FileExplorerReturnResult::Success) {
        return 1;
    }
    if (result == FileExplorerReturnResult::Cancelled) {
        return 3;
    }
    return 2;
}

FileExplorerStatusIconKind iconForOperation(const FileExplorerOperationType operation) {
    // Files StatusCenterItem ctor icon switch (default falls back to Delete).
    switch (operation) {
    case FileExplorerOperationType::Copy:
        return FileExplorerStatusIconKind::Copy;
    case FileExplorerOperationType::Move:
        return FileExplorerStatusIconKind::Move;
    case FileExplorerOperationType::Recycle:
        return FileExplorerStatusIconKind::Recycle;
    case FileExplorerOperationType::Extract:
        return FileExplorerStatusIconKind::Extract;
    case FileExplorerOperationType::Compressed:
        return FileExplorerStatusIconKind::Compress;
    case FileExplorerOperationType::Delete:
    case FileExplorerOperationType::Prepare:
    default:
        return FileExplorerStatusIconKind::Delete;
    }
}

QString withoutTrailingSeparators(const QString& path) {
    QString trimmed = path;
    while (trimmed.endsWith(QLatin1Char('/')) || trimmed.endsWith(QLatin1Char('\\'))) {
        trimmed.chop(1);
    }
    return trimmed;
}

QString lastPathComponent(const QString& path) {
    const QString trimmed = withoutTrailingSeparators(path);
    const qsizetype slash = std::max(trimmed.lastIndexOf(QLatin1Char('/')),
                                     trimmed.lastIndexOf(QLatin1Char('\\')));
    return slash < 0 ? trimmed : trimmed.mid(slash + 1);
}

QString parentDirectoryName(const QString& path) {
    const QString trimmed = withoutTrailingSeparators(path);
    const qsizetype slash = std::max(trimmed.lastIndexOf(QLatin1Char('/')),
                                     trimmed.lastIndexOf(QLatin1Char('\\')));
    return slash < 0 ? QString() : lastPathComponent(trimmed.left(slash));
}

QString itemsText(const qint64 count) {
    // Files ICU plural "{0, plural, one {# item} other {# items}}".
    return count == 1 ? QStringLiteral("1 item") : QStringLiteral("%1 items").arg(count);
}

bool operationCountsItems(const FileExplorerOperationType operation) {
    // Files ReportProgress footer switch: Recycle/Delete/Compressed report
    // "{0}/{1} items processed"; the rest report processed sizes.
    return operation == FileExplorerOperationType::Recycle ||
           operation == FileExplorerOperationType::Delete ||
           operation == FileExplorerOperationType::Compressed;
}

bool operationHasDiscoveryHeader(const FileExplorerOperationType operation) {
    // Files UpdateCardStrings discovery substitution exists for
    // Copy/Move/Delete (Recycle shares the Delete strings).
    return operation == FileExplorerOperationType::Copy ||
           operation == FileExplorerOperationType::Move ||
           operation == FileExplorerOperationType::Delete ||
           operation == FileExplorerOperationType::Recycle;
}

}  // namespace

FileExplorerStatusProgressReporter::FileExplorerStatusProgressReporter(Sink sink, Clock clock)
    : m_sink(std::move(sink)), m_clock(std::move(clock)) {
    if (!m_clock) {
        m_clock = []() {
            return QDateTime::currentMSecsSinceEpoch();
        };
    }
    // Files seeds _previousReportTime one second before StartTime so the very
    // first speed sample has a sane denominator.
    m_previous_speed_ms = m_clock() - 1000;
}

void FileExplorerStatusProgressReporter::setStatus(const FileExplorerReturnResult status) {
    if (m_progress.status != status) {
        m_progress.status = status;
        m_critical = true;
    }
}

void FileExplorerStatusProgressReporter::setEnumerationCompleted() {
    if (!m_progress.enumeration_completed) {
        m_progress.enumeration_completed = true;
        m_critical = true;
    }
}

void FileExplorerStatusProgressReporter::setFileName(const QString& file_name) {
    m_progress.file_name = file_name;
}

void FileExplorerStatusProgressReporter::setTotalSize(const qint64 total_size) {
    m_progress.total_size = total_size;
}

void FileExplorerStatusProgressReporter::setItemsCount(const qint64 items_count) {
    m_progress.items_count = items_count;
}

void FileExplorerStatusProgressReporter::setProcessedSize(const qint64 processed_size) {
    m_progress.processed_size = processed_size;
}

void FileExplorerStatusProgressReporter::addProcessedItems(const qint64 delta) {
    m_progress.processed_items_count += delta;
}

void FileExplorerStatusProgressReporter::computeSpeeds(const qint64 now_ms) {
    if (now_ms - m_previous_speed_ms < kSamplerIntervalMs) {
        return;
    }
    const double elapsed_seconds = static_cast<double>(now_ms - m_previous_speed_ms) / 1000.0;
    if (elapsed_seconds <= 0.0) {
        return;
    }
    // Files: delta since the previous sample over the elapsed wall time.
    m_progress.size_speed =
        static_cast<double>(m_progress.processed_size - m_previous_processed_size) /
        elapsed_seconds;
    m_progress.items_speed =
        static_cast<double>(m_progress.processed_items_count - m_previous_processed_items) /
        elapsed_seconds;
    m_previous_speed_ms = now_ms;
    m_previous_processed_size = m_progress.processed_size;
    m_previous_processed_items = m_progress.processed_items_count;
}

void FileExplorerStatusProgressReporter::report() {
    // Files auto-success: everything enumerated and fully processed flips the
    // status without an explicit call.
    if (m_progress.enumeration_completed && m_progress.total_size != 0 &&
        m_progress.processed_size == m_progress.total_size &&
        m_progress.processed_items_count == m_progress.items_count &&
        m_progress.status == FileExplorerReturnResult::InProgress) {
        setStatus(FileExplorerReturnResult::Success);
    }
    const qint64 now_ms = m_clock();
    computeSpeeds(now_ms);
    if (m_reported_once && !m_critical && now_ms - m_last_report_ms < kSamplerIntervalMs) {
        return;
    }
    m_reported_once = true;
    m_critical = false;
    m_last_report_ms = now_ms;
    if (m_sink) {
        m_sink(m_progress);
    }
}

void FileExplorerStatusProgressReporter::flushReport() {
    m_critical = true;
    report();
}

const FileExplorerStatusProgress& FileExplorerStatusProgressReporter::current() const {
    return m_progress;
}

FileExplorerStatusCenterItem::FileExplorerStatusCenterItem(
    const FileExplorerStatusCardRequest& request, QObject* parent)
    : QObject(parent) {
    applyInitialState(request);
    updateCardStrings();
}

void FileExplorerStatusCenterItem::applyInitialState(const FileExplorerStatusCardRequest& request) {
    m_operation = request.operation;
    m_return_result = request.result;
    m_source = request.source;
    m_destination = request.destination;
    m_total_items = request.items_count;
    m_total_size = request.total_size;
    m_message = tr("Discovering items...");
    switch (branchForResult(request.result)) {
    case 0:
        // Files: only the in-progress branch carries a cancellation source.
        m_cancelable = request.cancelable;
        m_in_progress = true;
        m_speed_available = request.can_provide_progress;
        m_indeterminate = !request.can_provide_progress;
        m_kind = FileExplorerStatusItemKind::InProgress;
        m_icon_kind = iconForOperation(request.operation);
        break;
    case 1:
        m_kind = FileExplorerStatusItemKind::Successful;
        m_icon_kind = FileExplorerStatusIconKind::Successful;
        break;
    case 3:
        m_kind = FileExplorerStatusItemKind::Canceled;
        m_icon_kind = iconForOperation(request.operation);
        break;
    default:
        m_kind = FileExplorerStatusItemKind::Error;
        m_icon_kind = FileExplorerStatusIconKind::Error;
        break;
    }
}

void FileExplorerStatusCenterItem::setExpanded(const bool expanded) {
    if (m_expanded == expanded) {
        return;
    }
    m_expanded = expanded;
    Q_EMIT changed();
}

void FileExplorerStatusCenterItem::reportProgress(const FileExplorerStatusProgress& progress) {
    if (m_cancel_requested) {
        // Files ReportProgress bails once the token is canceled.
        return;
    }
    m_return_result = progress.status;
    m_current_item_name = progress.file_name;
    applyProgressMessage(progress);
    updateCardStrings();
    applyProgressPercentAndSpeed(progress);
    Q_EMIT changed();
}

void FileExplorerStatusCenterItem::applyProgressMessage(
    const FileExplorerStatusProgress& progress) {
    if (operationCountsItems(m_operation)) {
        m_message = tr("%1/%2 processed")
                        .arg(progress.processed_items_count)
                        .arg(itemsText(progress.items_count));
    } else {
        m_message = tr("%1 of %2 processed")
                        .arg(formatBytes(progress.processed_size),
                             formatBytes(progress.total_size));
    }
    // Files grows the card totals monotonically.
    m_total_items = std::max(m_total_items, progress.items_count);
    m_total_size = std::max(m_total_size, progress.total_size);
    if (progress.enumeration_completed && m_discovering) {
        m_discovering = false;
        m_message = tr("Processing items...");
    }
    if (progress.enumeration_completed) {
        m_enumeration_completed = true;
    }
}

void FileExplorerStatusCenterItem::applyProgressPercentAndSpeed(
    const FileExplorerStatusProgress& progress) {
    double graph_speed = 0.0;
    if (progress.total_size > 0) {
        m_progress_percentage = static_cast<int>(
            std::clamp<qint64>(progress.processed_size * 100 / progress.total_size, 0, 100));
        m_speed_text = tr("%1/s").arg(formatBytes(static_cast<qint64>(progress.size_speed)));
        graph_speed = progress.size_speed;
    } else if (progress.items_count > 0) {
        m_progress_percentage = static_cast<int>(std::clamp<qint64>(
            progress.processed_items_count * 100 / progress.items_count, 0, 100));
        m_speed_text = tr("%1 items/s").arg(progress.items_speed, 0, 'f', 0);
        graph_speed = progress.items_speed;
    } else {
        m_speed_text = tr("N/A");
    }
    appendGraphPoint(m_progress_percentage, graph_speed);
    if (m_enumeration_completed && !m_indeterminate) {
        // Files appends the live percentage to the rebuilt header.
        m_header += tr(" (%1%)").arg(m_progress_percentage);
    }
}

void FileExplorerStatusCenterItem::appendGraphPoint(const double percent, const double speed) {
    if (!m_graph_points.isEmpty() && percent - m_graph_points.last().x() <= kGraphMinPercentStep) {
        return;
    }
    m_graph_points.append(QPointF(percent, speed));
}

void FileExplorerStatusCenterItem::updateCardStrings() {
    const int branch = branchForResult(m_return_result);
    m_header = headerText(tr(patternsFor(m_operation).branches[branch]));
    if (m_discovering && m_total_items > 0 && m_in_progress &&
        operationHasDiscoveryHeader(m_operation)) {
        m_header = tr("Discovered %1").arg(itemsText(m_total_items));
    }
    if (branch == 2) {
        m_sub_header = failedSubHeader();
    }
}

QString FileExplorerStatusCenterItem::headerText(const QString& pattern) const {
    if (m_operation == FileExplorerOperationType::Prepare) {
        return pattern;
    }
    if (m_operation == FileExplorerOperationType::Extract) {
        const QString source_name = m_source.isEmpty() ? QString()
                                                       : lastPathComponent(m_source.first());
        const QString destination_name =
            m_destination.isEmpty() ? QString() : lastPathComponent(m_destination.first());
        return pattern.arg(source_name, destination_name);
    }
    if (m_operation == FileExplorerOperationType::Delete ||
        m_operation == FileExplorerOperationType::Recycle) {
        // Files Delete headers name the folder the items are deleted from.
        const QString parent_name = m_source.isEmpty() ? QString()
                                                       : parentDirectoryName(m_source.first());
        return pattern.arg(itemsText(m_total_items), parent_name);
    }
    const QString destination_name =
        m_destination.isEmpty() ? QString() : lastPathComponent(m_destination.first());
    return pattern.arg(itemsText(m_total_items), destination_name);
}

QString FileExplorerStatusCenterItem::failedSubHeader() const {
    // Files StatusCenter_{Copy,Move,Delete}Failed_SubHeader templates; the
    // other operations carry no failed subheader.
    const QString source_path = m_source.isEmpty() ? QString() : m_source.first();
    const QString destination_path = m_destination.isEmpty() ? QString() : m_destination.first();
    if (m_operation == FileExplorerOperationType::Copy) {
        return tr("Failed to copy %1 from \"%2\" to \"%3\"")
            .arg(itemsText(m_total_items), source_path, destination_path);
    }
    if (m_operation == FileExplorerOperationType::Move) {
        return tr("Failed to move %1 from \"%2\" to \"%3\"")
            .arg(itemsText(m_total_items), source_path, destination_path);
    }
    if (m_operation == FileExplorerOperationType::Delete ||
        m_operation == FileExplorerOperationType::Recycle) {
        return tr("Failed to delete %1").arg(itemsText(m_total_items));
    }
    return QString();
}

void FileExplorerStatusCenterItem::requestCancel() {
    if (!m_cancelable) {
        return;
    }
    // Files ExecuteCancelCommand.
    m_cancel_requested = true;
    m_cancelable = false;
    m_indeterminate = true;
    m_expanded = false;
    m_speed_available = false;
    m_header = tr("Canceling - %1").arg(m_header);
    Q_EMIT cancelRequested();
    Q_EMIT changed();
}

FileExplorerStatusCenterModel::FileExplorerStatusCenterModel(QObject* parent) : QObject(parent) {}

FileExplorerStatusCenterItem* FileExplorerStatusCenterModel::addItem(
    const FileExplorerStatusCardRequest& request) {
    auto* item = new FileExplorerStatusCenterItem(request, this);
    // Files AddItem inserts at index 0 so the newest card renders on top.
    m_items.prepend(item);
    connect(item,
            &FileExplorerStatusCenterItem::changed,
            this,
            &FileExplorerStatusCenterModel::notifyChanges);
    Q_EMIT itemAdded(item);
    notifyChanges();
    return item;
}

void FileExplorerStatusCenterModel::removeItem(FileExplorerStatusCenterItem* item) {
    if (!m_items.removeOne(item)) {
        return;
    }
    item->deleteLater();
    notifyChanges();
}

void FileExplorerStatusCenterModel::removeAllCompletedItems() {
    const QList<FileExplorerStatusCenterItem*> snapshot = m_items;
    for (FileExplorerStatusCenterItem* item : snapshot) {
        if (!item->isInProgress()) {
            m_items.removeOne(item);
            item->deleteLater();
        }
    }
    notifyChanges();
}

int FileExplorerStatusCenterModel::inProgressCount() const {
    return static_cast<int>(
        std::count_if(m_items.begin(), m_items.end(), [](const FileExplorerStatusCenterItem* item) {
            return item->isInProgress();
        }));
}

int FileExplorerStatusCenterModel::averageProgress() const {
    int sum = 0;
    int count = 0;
    for (const FileExplorerStatusCenterItem* item : m_items) {
        if (item->isInProgress()) {
            sum += item->progressPercentage();
            ++count;
        }
    }
    return count > 0 ? sum / count : 0;
}

int FileExplorerStatusCenterModel::infoBadgeState() const {
    const bool any_failure =
        std::any_of(m_items.begin(), m_items.end(), [](const FileExplorerStatusCenterItem* item) {
            return item->returnResult() != FileExplorerReturnResult::InProgress &&
                   item->returnResult() != FileExplorerReturnResult::Success;
        });
    const bool any_in_progress = hasAnyItemInProgress();
    if (!any_failure) {
        return any_in_progress ? 1 : 0;
    }
    return any_in_progress ? 2 : 3;
}

int FileExplorerStatusCenterModel::infoBadgeValue() const {
    const int count = inProgressCount();
    return count > 0 ? count : -1;
}

void FileExplorerStatusCenterModel::flyoutOpened() {
    m_show_progress_ring = hasAnyItemInProgress() || infoBadgeState() == 3;
    Q_EMIT changed();
}

void FileExplorerStatusCenterModel::notifyChanges() {
    if (hasAnyItemInProgress()) {
        m_show_progress_ring = true;
    }
    Q_EMIT changed();
}

}  // namespace sak
