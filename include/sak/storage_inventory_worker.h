// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file storage_inventory_worker.h
/// @brief Storage inventory collection and parsing for Partition Manager.

#pragma once

#include "sak/partition_manager_types.h"
#include "sak/process_runner.h"

#include <QElapsedTimer>
#include <QObject>

#include <atomic>
#include <expected>
#include <functional>
#include <mutex>

namespace sak {

struct StorageInventoryScanResult {
    PartitionInventory inventory;
    bool cancelled{false};
};

/// A scan with no progress line for this long has stopped answering. 90 s, as long as the longest
/// single call may wait (kInventoryQueryCapSeconds): on a PC under heavy disk load, Windows'
/// storage service took long enough that 30 s stopped four scans in a row, and one of them failed
/// the verification of a Move that had finished (operation proof, 2026-11-12, F26).
inline constexpr int kInventoryScanIdleLimitMs = 90'000;
/// While a scan is quiet, the log says so this often: how long, and at which step.
inline constexpr int kInventoryScanQuietLogMs = 15'000;
/// However much progress it makes, a scan ends here.
inline constexpr int kInventoryScanCeilingMs = 300'000;
/// The longest one call in a scan may wait. Right after a clone's raw write took a disk offline and
/// online, the Storage WMI provider took 36 s to load again before Get-PhysicalDisk answered
/// (2026-11-09); this is that, with room.
inline constexpr int kInventoryQueryCapSeconds = 90;
/// How often a call that is still waiting says so, well inside the idle limit.
inline constexpr int kInventoryHeartbeatSeconds = 5;
/// The exit code of a scan that stopped at a step it cannot go on without.
inline constexpr int kInventoryScanDidNotFinishExitCode = 3;

/// How far a scan has got: the disk it is on, of how many.
struct InventoryScanProgress {
    int disk{0};
    int disks{0};
};
using InventoryScanProgressCallback = std::function<void(const InventoryScanProgress&)>;

struct InventoryScanLimits {
    int idle_ms{kInventoryScanIdleLimitMs};
    int ceiling_ms{kInventoryScanCeilingMs};
    int quiet_log_ms{kInventoryScanQuietLogMs};
};

/// Watches the progress lines a scan script writes to standard error ("SAK-SCAN-PROGRESS step").
/// A slow PC keeps scanning while it keeps moving; one that goes quiet is stopped, and the scan
/// says it did not finish rather than that the PC has no disks. One thread may feed it while
/// another asks whether it has gone quiet.
class InventoryScanLiveness {
public:
    explicit InventoryScanLiveness(InventoryScanLimits limits,
                                   InventoryScanProgressCallback on_progress = {});
    /// Starts the quiet clock. Until then nothing counts as quiet: a helper waiting on the UAC
    /// prompt is not a scan that stopped.
    void arm();
    void feed(const QString& stderr_chunk);
    [[nodiscard]] bool idleExpired() const;
    /// Every quiet_log_ms of quiet, a line in the log: how long the scan has been quiet, at which
    /// step, and when it will be stopped. Called as often as idleExpired. The line, or empty.
    QString reportQuiet();
    /// This watcher stopped the run.
    void markStopped();
    /// What the run's result means for a scan. Stopped for going quiet, or past its ceiling, it did
    /// not finish, and its error says so and names the last step. Progress lines leave the error.
    void settle(ProcessResult* result) const;
    [[nodiscard]] static QString withoutProgressLines(const QString& text);

private:
    mutable std::mutex m_mutex;
    InventoryScanLimits m_limits;
    InventoryScanProgressCallback m_on_progress;
    QElapsedTimer m_since_progress;
    QString m_pending;
    QString m_last_step;
    bool m_armed{false};
    bool m_stopped{false};
    qint64 m_quiet_reported_ms{0};
};

/// How the inventory script runs for a scan that needs administrator rights.
struct PrivilegedInventoryRunners {
    std::function<ProcessResult()> in_this_process;
    /// The result, or why the helper could not run it.
    std::function<std::expected<ProcessResult, QString>()> in_helper;
};

/// Runs the script where it can read everything: here if this process is elevated, otherwise in the
/// helper. If the helper could not run it, the script runs here, so the panel keeps its list, and
/// @p notes says what stopped it. A note makes the inventory fail Apply's preflight.
[[nodiscard]] ProcessResult runPrivilegedInventoryScript(bool process_is_elevated,
                                                         const PrivilegedInventoryRunners& runners,
                                                         QStringList* notes);

class StorageInventoryWorker : public QObject {
    Q_OBJECT

public:
    explicit StorageInventoryWorker(QObject* parent = nullptr);

    [[nodiscard]] PartitionInventory scan();
    void cancel() noexcept;

    /// Scan disks/partitions/volumes on this machine.
    /// @param allow_elevation when false, raw file-system probing of foreign
    /// partitions never escalates via the elevation broker (no UAC prompt, no
    /// elevated helper): such partitions simply keep an empty file_system and a
    /// warning. Headless/read-only callers (e.g. the AI assistant) pass false so
    /// a "read-only" inventory can never silently trigger a privilege escalation.
    [[nodiscard]] static PartitionInventory scanCurrentSystem(
        bool allow_elevation = true, const CancelCheck& should_cancel = {});
    [[nodiscard]] static StorageInventoryScanResult scanCurrentSystemResult(
        bool allow_elevation = true, const CancelCheck& should_cancel = {});
    /// What Partition Manager shows after Scan Disks. It never asks for elevation: a person who
    /// only wants to look at disks gets no prompt. Without administrator rights it cannot read
    /// BitLocker, a volume's dirty bit or a raw partition, and marks them pending, not failed.
    [[nodiscard]] static StorageInventoryScanResult scanForBrowsing(
        const CancelCheck& should_cancel = {},
        const InventoryScanProgressCallback& on_progress = {});
    /// What scanForBrowsing does with its script, for any script and limits. Public so a test can
    /// run a stand-in that reads no disk.
    [[nodiscard]] static StorageInventoryScanResult runBrowsingScanScript(
        const QString& script,
        const InventoryScanLimits& limits,
        const CancelCheck& should_cancel,
        const InventoryScanProgressCallback& on_progress);
    /// Runs a scan script in this process, and stops it when it goes quiet or reaches its ceiling.
    [[nodiscard]] static ProcessResult runInventoryScript(
        const QString& script,
        const InventoryScanLimits& limits,
        const CancelCheck& should_cancel,
        const InventoryScanProgressCallback& on_progress);
    /// PowerShell functions Invoke-SakBoundedQuery (a storage query run as a job) and
    /// Invoke-SakBoundedCall (any other call, in a runspace of its own): each gives a call a set
    /// time, says every few seconds that it is still waiting, and reports a call that ran out of
    /// time by its step. Stop-SakScan ends a scan that cannot go on. Public for a test.
    [[nodiscard]] static QString boundedQueryFunctionScript();
    /// The scan a press of Apply runs before it writes anything, and again afterwards. It runs in
    /// the session's elevated helper, whose start is the job's one prompt, so it reads what the
    /// browsing scan could not, right before the write. If the helper cannot run it, the prompt
    /// having been declined for one, the inventory says so and Apply stops.
    [[nodiscard]] static StorageInventoryScanResult scanForApply(
        const CancelCheck& should_cancel = {},
        const InventoryScanProgressCallback& on_progress = {});
    [[nodiscard]] static PartitionInventory parseInventoryJson(const QByteArray& json_data);
    /// The same parse for a scan made under a standard-user token, as scanForBrowsing's is.
    [[nodiscard]] static PartitionInventory parseStandardUserInventoryJson(
        const QByteArray& json_data);
    /// What a queue is checked against: the same whoever scanned, elevated or not.
    [[nodiscard]] static QString layoutHash(const PartitionInventory& inventory);
    /// @param read_administrator_state false for a process without administrator rights: it then
    ///        skips the BitLocker, dirty-bit and reliability reads Windows refuses it anyway. Each
    ///        refused BitLocker read took five seconds, so five drive letters ran out the time.
    [[nodiscard]] static QString inventoryPowerShellScript(bool read_administrator_state = true);
    /// The warnings for volumes whose dirty bit or BitLocker state the scan could not read. A
    /// scan adds them after its raw probe, which is what tells a volume Windows shows as RAW
    /// because it holds ext, HFS+, APFS or swap (nothing to ask fsutil) from an unreadable one.
    static void appendVolumeStateWarnings(PartitionInventory* inventory);

Q_SIGNALS:
    void inventoryReady(const sak::PartitionInventory& inventory);
    void inventoryError(const QString& message);

private:
    enum class ReadLevel {
        Everything,
        WhatAStandardUserCanRead
    };
    [[nodiscard]] static StorageInventoryScanResult finishScan(const ProcessResult& result,
                                                               ReadLevel level,
                                                               bool allow_elevation,
                                                               const CancelCheck& should_cancel);
    enum class VolumeStateWarnings {
        Now,
        Later
    };
    [[nodiscard]] static PartitionInventory parseInventory(
        const QByteArray& json_data,
        ReadLevel level,
        VolumeStateWarnings warnings = VolumeStateWarnings::Now);

    std::atomic_bool m_cancelled{false};
};

}  // namespace sak
