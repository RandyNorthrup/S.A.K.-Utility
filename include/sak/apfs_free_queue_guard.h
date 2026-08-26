// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file apfs_free_queue_guard.h
/// @brief F25: refuse a checkpoint whose free queue would release live container metadata.
///
/// An APFS free-queue record schedules ghost blocks for reuse. A record covering live container
/// metadata therefore hands the allocator the checkpoint ring, the internal pool or the pool's
/// bitmap ring, and the next allocation overwrites them -- silent, unrecoverable corruption of a
/// mounted container.
///
/// The guard is lifted out of partition_apfs_writer.cpp's anonymous namespace so it can be
/// exercised directly with hostile runs. That matters more than usual here: F25's FIRST
/// implementation was written, reverted, and then re-written wrong twice more, every time as a
/// FALSE-CLOSE that rejected a legitimate commit, and every time caught only by a full build plus
/// the round-trip tests. The three mistakes were:
///
///   1. testing against PRE-commit geometry, so a chunk-adding grow -- which re-homes the whole
///      internal pool and then queues the pool's OLD location -- was rejected for freeing blocks
///      that are reserved before the commit and free after it;
///   2. reserving the internal pool against the INTERNAL-POOL queue, whose records are by
///      definition ghost blocks inside that pool (the rotated cib-0 slot) and inside its bitmap
///      ring (the overflow tier's old boundary-chunk bitmap), so every ordinary rotation commit
///      was rejected;
///   3. reading sm_ip_bm_block_count -- a uint32 field four bytes below sm_ip_bm_base -- as a
///      uint64, which swallowed the base and reported the bitmap ring as spanning 725 billion
///      blocks, rejecting a legitimate main-device record two blocks past the pool.
///
/// The caller supplies the region set, already resolved to POST-commit geometry and already
/// scoped to the queue being checked. This seam owns only the overlap arithmetic and the refusal.

#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <limits>

namespace sak::apfs {

/// One free-queue record: `length` blocks of ghost space starting at `paddr`.
struct FreeQueueRun {
    uint64_t paddr{0};
    uint64_t length{0};
};

/// One region of the container a free-queue record must never cover.
struct ReservedRegion {
    uint64_t start{0};
    uint64_t blocks{0};
    QString name;
};

namespace detail {

/// Half-open end of [start, start+length), or false when the span would wrap the address
/// space. Failing closed here matters: an end computed BELOW its own start makes every
/// overlap test silently false and turns the guard into a no-op for the most malformed input.
[[nodiscard]] inline bool spanEnd(uint64_t start, uint64_t length, uint64_t& endOut) {
    if (start > std::numeric_limits<uint64_t>::max() - length) {
        return false;
    }
    endOut = start + length;
    return true;
}

/// True when the single run [run.paddr, runEnd) clears every region in @p reserved.
[[nodiscard]] inline bool runAvoidsReserved(const FreeQueueRun& run,
                                            uint64_t runEnd,
                                            const QString& which,
                                            const QVector<ReservedRegion>& reserved,
                                            QStringList* blockers) {
    for (const ReservedRegion& region : reserved) {
        if (region.blocks == 0) {
            continue;
        }
        uint64_t regionEnd = 0;
        if (!spanEnd(region.start, region.blocks, regionEnd)) {
            if (blockers != nullptr) {
                blockers->append(QStringLiteral("APFS reserved region %1 at block %2 length "
                                                "%3 overflows the address space")
                                     .arg(region.name)
                                     .arg(region.start)
                                     .arg(region.blocks));
            }
            return false;
        }
        // Half-open overlap: [a,b) meets [c,d) iff a < d and c < b.
        if (run.paddr < regionEnd && region.start < runEnd) {
            if (blockers != nullptr) {
                blockers->append(
                    QStringLiteral("APFS %1 free-queue would release live metadata: run "
                                   "[%2,%3) overlaps the %4 at [%5,%6)")
                        .arg(which)
                        .arg(run.paddr)
                        .arg(runEnd)
                        .arg(region.name)
                        .arg(region.start)
                        .arg(regionEnd));
            }
            return false;
        }
    }
    return true;
}

}  // namespace detail

/// True when no run in @p runs overlaps any region in @p reserved. On the first overlap this
/// appends a blocker naming the run, the region and both spans, and returns false -- the caller
/// must not publish the checkpoint. @p which names the queue for the message ("internal-pool" /
/// "main-device").
[[nodiscard]] inline bool freeQueueRunsAvoidReserved(const QVector<FreeQueueRun>& runs,
                                                     const QString& which,
                                                     const QVector<ReservedRegion>& reserved,
                                                     QStringList* blockers) {
    for (const FreeQueueRun& run : runs) {
        // A zero-length run covers nothing. It is not this guard's business to reject it; the
        // tree builders drop empty records on their own.
        if (run.length == 0) {
            continue;
        }
        uint64_t runEnd = 0;
        if (!detail::spanEnd(run.paddr, run.length, runEnd)) {
            if (blockers != nullptr) {
                blockers->append(QStringLiteral("APFS %1 free-queue run at block %2 length %3 "
                                                "overflows the address space")
                                     .arg(which)
                                     .arg(run.paddr)
                                     .arg(run.length));
            }
            return false;
        }
        if (!detail::runAvoidsReserved(run, runEnd, which, reserved, blockers)) {
            return false;
        }
    }
    return true;
}

}  // namespace sak::apfs
