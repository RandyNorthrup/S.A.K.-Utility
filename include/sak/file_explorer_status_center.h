// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_status_center.h
/// @brief Files StatusCenter models: per-card state, throttled progress
/// reporting, and the aggregate view-model (Files StatusCenterItem.cs,
/// StatusCenterItemProgressModel.cs, StatusCenterViewModel.cs).

#pragma once

#include <QMetaType>
#include <QObject>
#include <QPointF>
#include <QString>
#include <QStringList>

#include <functional>

namespace sak {

/// Files ReturnResult (Data/Enums/ReturnResult.cs).
enum class FileExplorerReturnResult {
    InProgress,
    Success,
    Failed,
    IntegrityCheckFailed,
    UnknownException,
    BadArgumentException,
    NullException,
    AccessUnauthorized,
    Cancelled,
};

/// Files FileOperationType (Data/Enums/FileOperationType.cs), the subset that
/// posts status-center cards in this explorer.
enum class FileExplorerOperationType {
    Copy,
    Move,
    Extract,
    Recycle,
    Delete,
    Compressed,
    Prepare,
};

/// Files StatusCenterItemKind (Data/Enums/StatusCenterItemKind.cs).
enum class FileExplorerStatusItemKind {
    InProgress,
    Successful,
    Error,
    Canceled,
};

/// Files StatusCenterItemIconKind (Data/Enums/StatusCenterItemIconKind.cs),
/// dropped to the operations this explorer surfaces.
enum class FileExplorerStatusIconKind {
    Copy,
    Move,
    Delete,
    Recycle,
    Extract,
    Compress,
    Successful,
    Error,
};

/// One progress payload snapshot (Files StatusCenterItemProgressModel fields).
/// Value type so worker threads can queue it across to the GUI thread.
struct FileExplorerStatusProgress {
    FileExplorerReturnResult status = FileExplorerReturnResult::InProgress;
    bool enumeration_completed = false;
    QString file_name;
    qint64 total_size = 0;
    qint64 processed_size = 0;
    qint64 items_count = 0;
    qint64 processed_items_count = 0;
    /// Bytes per second (Files ProcessingSizeSpeed).
    double size_speed = 0.0;
    /// Items per second (Files ProcessingItemsCountSpeed).
    double items_speed = 0.0;
};

/// Worker-side accumulator that throttles snapshot delivery to the sink at the
/// Files sampler interval (100ms) unless a critical field changed, and derives
/// speeds from the delta since the previous sampled report (Files
/// StatusCenterItemProgressModel.Report + IntervalSampler).
class FileExplorerStatusProgressReporter {
public:
    using Sink = std::function<void(const FileExplorerStatusProgress&)>;
    using Clock = std::function<qint64()>;

    /// @param sink Receives throttled snapshots (on the calling thread).
    /// @param clock Millisecond clock; tests inject a fake, production uses
    /// the monotonic default.
    explicit FileExplorerStatusProgressReporter(Sink sink, Clock clock = {});

    /// Critical: reports immediately on change (Files Status setter).
    void setStatus(FileExplorerReturnResult status);
    /// Critical: reports immediately on change (Files EnumerationCompleted).
    void setEnumerationCompleted();
    void setFileName(const QString& file_name);
    void setTotalSize(qint64 total_size);
    void setItemsCount(qint64 items_count);
    void setProcessedSize(qint64 processed_size);
    void addProcessedItems(qint64 delta);

    /// Delivers the current snapshot if 100ms elapsed since the last delivery
    /// or a critical field changed (Files Report()).
    void report();
    /// Delivers the current snapshot unconditionally (terminal flush so the
    /// final counts always reach the card).
    void flushReport();

    [[nodiscard]] const FileExplorerStatusProgress& current() const;

private:
    void computeSpeeds(qint64 now_ms);

    Sink m_sink;
    Clock m_clock;
    FileExplorerStatusProgress m_progress;
    bool m_critical = false;
    qint64 m_last_report_ms = 0;
    bool m_reported_once = false;
    qint64 m_previous_speed_ms = 0;
    qint64 m_previous_processed_size = 0;
    qint64 m_previous_processed_items = 0;
};

/// Card construction arguments (Files StatusCenterViewModel.AddItem
/// parameters; a cancellation source is supplied only for in-progress cards).
struct FileExplorerStatusCardRequest {
    FileExplorerReturnResult result = FileExplorerReturnResult::InProgress;
    FileExplorerOperationType operation = FileExplorerOperationType::Copy;
    QStringList source;
    QStringList destination;
    bool can_provide_progress = false;
    qint64 items_count = 0;
    qint64 total_size = 0;
    bool cancelable = false;
};

/// One status-center card (Files StatusCenterItem).
class FileExplorerStatusCenterItem : public QObject {
    Q_OBJECT

public:
    explicit FileExplorerStatusCenterItem(const FileExplorerStatusCardRequest& request,
                                          QObject* parent = nullptr);

    [[nodiscard]] FileExplorerStatusItemKind kind() const { return m_kind; }
    [[nodiscard]] FileExplorerStatusIconKind iconKind() const { return m_icon_kind; }
    [[nodiscard]] FileExplorerOperationType operation() const { return m_operation; }
    [[nodiscard]] FileExplorerReturnResult returnResult() const { return m_return_result; }
    [[nodiscard]] QString header() const { return m_header; }
    [[nodiscard]] QString subHeader() const { return m_sub_header; }
    [[nodiscard]] QString message() const { return m_message; }
    [[nodiscard]] QString speedText() const { return m_speed_text; }
    [[nodiscard]] QString currentItemName() const { return m_current_item_name; }
    [[nodiscard]] int progressPercentage() const { return m_progress_percentage; }
    [[nodiscard]] bool isInProgress() const { return m_in_progress; }
    [[nodiscard]] bool isDiscovering() const { return m_discovering; }
    [[nodiscard]] bool isIndeterminate() const { return m_indeterminate; }
    [[nodiscard]] bool isCancelable() const { return m_cancelable; }
    [[nodiscard]] bool isCancelRequested() const { return m_cancel_requested; }
    [[nodiscard]] bool isSpeedAndProgressAvailable() const { return m_speed_available; }
    [[nodiscard]] bool isExpanded() const { return m_expanded; }
    void setExpanded(bool expanded);
    [[nodiscard]] qint64 totalSize() const { return m_total_size; }
    [[nodiscard]] qint64 totalItemsCount() const { return m_total_items; }
    /// Speed graph points, x = progress percent, y = speed
    /// (Files SpeedGraphValues).
    [[nodiscard]] const QList<QPointF>& graphPoints() const { return m_graph_points; }

    /// Applies one progress snapshot (Files StatusCenterItem.ReportProgress);
    /// no-op once cancel was requested.
    void reportProgress(const FileExplorerStatusProgress& progress);

    /// Files ExecuteCancelCommand: flags cancellation, drops the card to an
    /// indeterminate non-cancelable collapsed state, and prefixes the header
    /// with "Canceling - ".
    void requestCancel();

Q_SIGNALS:
    void changed();
    void cancelRequested();

private:
    void applyInitialState(const FileExplorerStatusCardRequest& request);
    void applyTerminalTransition();
    void applyProgressMessage(const FileExplorerStatusProgress& progress);
    void applyProgressPercentAndSpeed(const FileExplorerStatusProgress& progress);
    void appendGraphPoint(double percent, double speed);
    void updateCardStrings();
    [[nodiscard]] QString headerText(const QString& pattern) const;
    [[nodiscard]] QString failedSubHeader() const;

    FileExplorerStatusItemKind m_kind = FileExplorerStatusItemKind::InProgress;
    FileExplorerStatusIconKind m_icon_kind = FileExplorerStatusIconKind::Copy;
    FileExplorerOperationType m_operation = FileExplorerOperationType::Copy;
    FileExplorerReturnResult m_return_result = FileExplorerReturnResult::InProgress;
    QStringList m_source;
    QStringList m_destination;
    QString m_header;
    QString m_sub_header;
    QString m_message;
    QString m_speed_text;
    QString m_current_item_name;
    int m_progress_percentage = 0;
    bool m_in_progress = false;
    bool m_discovering = true;
    bool m_indeterminate = false;
    bool m_cancelable = false;
    bool m_cancel_requested = false;
    bool m_speed_available = false;
    bool m_expanded = false;
    bool m_enumeration_completed = false;
    qint64 m_total_size = 0;
    qint64 m_total_items = 0;
    QList<QPointF> m_graph_points;
};

/// Aggregate collection + toolbar badge state (Files StatusCenterViewModel).
class FileExplorerStatusCenterModel : public QObject {
    Q_OBJECT

public:
    explicit FileExplorerStatusCenterModel(QObject* parent = nullptr);

    /// Inserts the new card at the top (Files AddItem inserts at index 0).
    FileExplorerStatusCenterItem* addItem(const FileExplorerStatusCardRequest& request);
    void removeItem(FileExplorerStatusCenterItem* item);
    /// Removes every card that is no longer in progress (Files
    /// RemoveAllCompletedItems).
    void removeAllCompletedItems();

    [[nodiscard]] const QList<FileExplorerStatusCenterItem*>& items() const { return m_items; }
    [[nodiscard]] bool hasAnyItem() const { return !m_items.isEmpty(); }
    [[nodiscard]] int inProgressCount() const;
    [[nodiscard]] bool hasAnyItemInProgress() const { return inProgressCount() > 0; }
    /// Arithmetic mean of in-progress card percentages (Files
    /// AverageOperationProgressValue).
    [[nodiscard]] int averageProgress() const;
    /// Files InfoBadgeState: 0 all successful, 1 in progress, 2 in progress
    /// with an error, 3 completed with an error.
    [[nodiscard]] int infoBadgeState() const;
    /// In-progress count while positive, otherwise -1 (Files InfoBadgeValue).
    [[nodiscard]] int infoBadgeValue() const;
    [[nodiscard]] bool showProgressRing() const { return m_show_progress_ring; }
    /// Files OnStatusCenterFlyoutOpened: the ring stays only while something
    /// runs or a completed error is still pending review.
    void flyoutOpened();

Q_SIGNALS:
    void itemAdded(sak::FileExplorerStatusCenterItem* item);
    void changed();

private:
    void notifyChanges();

    QList<FileExplorerStatusCenterItem*> m_items;
    bool m_show_progress_ring = false;
};

}  // namespace sak

Q_DECLARE_METATYPE(sak::FileExplorerStatusProgress)
