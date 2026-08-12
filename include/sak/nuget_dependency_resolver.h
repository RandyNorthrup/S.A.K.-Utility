// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file nuget_dependency_resolver.h
/// @brief Transport-agnostic transitive NuGet dependency resolver.
///
/// Walks a package's dependency graph, selecting for every node the highest
/// available version that satisfies the declared version range (B10-25), so an
/// offline deployment bundle actually contains the complete, compatible closure
/// needed to install without network access.
///
/// The resolver is a PULL-BASED STEP MACHINE and holds ALL of its state as
/// per-instance members -- there are NO shared/static members -- so two
/// concurrent resolutions simply use two instances and can never clobber each
/// other (B10-28 part 1: per-request state by construction). It performs no I/O
/// itself: the caller drives it, fetching each requested package's feed however
/// it likes (the live worker uses a synchronous network transfer; tests use an
/// in-memory map). That makes the entire graph algorithm unit-testable with no
/// network.
///
/// Usage:
///   NuGetDependencyResolver r;
///   r.start("googlechrome", "");            // "" root version = pick latest
///   while (!r.isComplete()) {
///       const QString id = r.nextFetchId();
///       // fetch OData FindPackagesById()?id='id' for this id...
///       if (ok) r.provideFeed(id, NuGetDependencyResolver::parseODataFeedVersions(xml));
///       else    r.provideFeedFailure(id);
///   }
///   const auto closure = r.resolved();       // root + every transitive dependency

#pragma once

#include "sak/nuget_version_range.h"

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

namespace sak {

/// @brief A single declared dependency: a package id plus its version range.
struct NuGetDependency {
    QString id;
    QString version_range;  ///< NuGet interval notation ("" = any)
};

/// @brief One available version of a package and the dependencies it declares.
struct FeedPackageVersion {
    QString version;
    QVector<NuGetDependency> dependencies;
};

/// @brief A concretely selected package in the resolved closure.
struct ResolvedPackage {
    QString package_id;
    QString version;           ///< the concrete version chosen from the feed
    QString version_range;     ///< the range that selected it (provenance)
    int depth{0};              ///< 0 = root, 1 = direct dependency, ...
    QStringList dependencies;  ///< direct dependency ids declared by the chosen version
};

/// @brief Transitive dependency resolver (pull-based, per-request state).
class NuGetDependencyResolver {
public:
    /// @param max_depth Maximum graph depth (root = 0). Deeper edges are dropped.
    /// @param max_packages Hard cap on resolved nodes (runaway-graph backstop).
    explicit NuGetDependencyResolver(int max_depth = kDefaultMaxDepth,
                                     int max_packages = kDefaultMaxPackages);

    /// @brief Begin resolution from a root package, clearing any prior state. An
    ///        empty @p root_version resolves the root to its latest; a specific
    ///        version pins it exactly.
    void start(const QString& root_id, const QString& root_version);

    /// @brief Add another root package to the SAME resolution WITHOUT clearing
    ///        state, so a bundle listing several packages resolves into one shared
    ///        closure (a dependency common to two roots is fetched once). A root
    ///        whose id was already scheduled is skipped. Call after start().
    void addRoot(const QString& root_id, const QString& root_version);

    /// @brief True while there is another package id awaiting a feed fetch.
    [[nodiscard]] bool isComplete() const { return m_queue.isEmpty(); }

    /// @brief The next package id the caller must fetch. Empty if complete.
    [[nodiscard]] QString nextFetchId() const;

    /// @brief The version range constraining the next fetch (for feed queries
    ///        that can filter server-side). Empty ("any") is common.
    [[nodiscard]] QString nextFetchRange() const;

    /// @brief Supply the feed result (all available versions + their deps) for
    ///        the id most recently returned by nextFetchId(). Selects the
    ///        highest-satisfying version, records it, and enqueues its
    ///        (unvisited) dependencies.
    void provideFeed(const QString& id, const QVector<FeedPackageVersion>& versions);

    /// @brief Record that the feed fetch for @p id failed; resolution continues.
    void provideFeedFailure(const QString& id);

    /// @brief Abort: drop all pending work (a cancelled operation).
    void cancel();

    /// @brief The resolved closure (root first, then in discovery order).
    [[nodiscard]] const QVector<ResolvedPackage>& resolved() const { return m_resolved; }

    /// @brief Non-fatal problems encountered (unsatisfiable ranges, fetch
    ///        failures, depth/size caps hit). A non-empty list means the bundle
    ///        may be incomplete -- the caller should surface it, not silently
    ///        claim success.
    [[nodiscard]] const QStringList& errors() const { return m_errors; }

    // ---- Pure OData / dependency-string parsing (shared, unit-tested) --------

    /// @brief Parse a NuGet v2 OData "Dependencies" string into (id, range)
    ///        pairs. Pipe-separated "id:versionRange:targetFramework" entries;
    ///        framework-only markers ("::net45") are skipped. The version range is
    ///        preserved (it is what drives highest-satisfying selection).
    [[nodiscard]] static QVector<NuGetDependency> parseDependencies(const QString& dep_string);

    /// @brief Parse an OData feed (FindPackagesById result) into the available
    ///        (version, dependencies) list the resolver consumes.
    [[nodiscard]] static QVector<FeedPackageVersion> parseODataFeedVersions(const QByteArray& xml);

    static constexpr int kDefaultMaxDepth = 10;
    static constexpr int kDefaultMaxPackages = 500;

private:
    struct QueueItem {
        QString id;
        QString version_range;
        int depth{0};
        bool is_root{false};
        QString root_version;  ///< only meaningful when is_root
    };

    /// @param contributor Provenance key ("<id>\x1f<version>") of the package
    ///        version whose edges these are, so a later reselection can withdraw
    ///        EXACTLY this contribution.
    void enqueueDependencies(const FeedPackageVersion& selected,
                             int depth,
                             const QString& contributor);
    [[nodiscard]] std::optional<FeedPackageVersion> selectVersion(
        const QueueItem& item, const QVector<FeedPackageVersion>& versions) const;

    /// @brief Index in m_queue of the pending item whose id matches @p id
    ///        (case-insensitive), or -1 if none. A feed/failure response is
    ///        applied to the item it actually names -- NEVER blindly to the queue
    ///        head -- so an out-of-order or mismatched response can never bind the
    ///        wrong package's version to a different id.
    [[nodiscard]] int pendingIndexForId(const QString& id) const;

    /// @brief Select + record the version for a dequeued item and enqueue its deps.
    void resolveDequeued(const QueueItem& item, const QVector<FeedPackageVersion>& versions);

    /// @brief Run the final constraint-validation pass exactly once, the first time
    ///        the work queue is observed empty (covers every provide* exit path).
    void maybeValidateOnDrain();

    /// @brief Record a declared version range for @p id (deduped). EVERY edge to a
    ///        package -- including a second diamond edge whose enqueue is skipped
    ///        because the id is already visited -- records its constraint here, so
    ///        selection and the final validation see the FULL constraint set.
    ///        @p contributor is the provenance key of the declaring package version
    ///        (or the root sentinel); it is tracked so a reselection can withdraw
    ///        this exact contribution without dropping a range another package
    ///        still declares.
    void recordConstraint(const QString& id, const QString& range, const QString& contributor);

    /// @brief True if @p version satisfies @p own_range AND every other recorded
    ///        constraint for @p id (so a diamond's second edge is honored).
    [[nodiscard]] bool satisfiesAllConstraints(const QString& id,
                                               const QString& own_range,
                                               const NuGetVersion& version) const;

    /// @brief After the queue drains, flag any resolved package whose chosen
    ///        version violates a constraint that was only recorded AFTER it was
    ///        selected AND that no reselection could satisfy (a genuine conflict).
    ///        Runs once.
    void validateConstraints();

    /// @brief Index of the resolved package with @p lower_id, or -1 if none.
    [[nodiscard]] int resolvedIndex(const QString& lower_id) const;

    /// @brief When a constraint recorded AFTER a package was resolved invalidates
    ///        its chosen version, re-run selection over that package's CACHED feed
    ///        honoring the full constraint set (bounded backtracking -- constraints
    ///        only tighten, so the chosen version is monotonically non-increasing
    ///        and the pass terminates). A resolvable diamond is thus corrected in
    ///        place instead of merely erroring after the fact; a truly
    ///        unsatisfiable diamond still leaves the conflict for validateConstraints.
    void reselectIfResolved(const QString& id);

    /// @brief Overwrite a resolved package with a re-selected feed version (new
    ///        version + dependency ids) and enqueue that version's dependencies.
    ///        Before applying, the SUPERSEDED version's own constraint/demand
    ///        contribution is withdrawn (see withdrawOldContribution) so no stale
    ///        edge from the version being replaced survives.
    void applyReselection(int idx, const FeedPackageVersion& chosen);

    /// @brief Withdraw every constraint and demand the pre-overwrite version of
    ///        @p pkg_id contributed, derived from its cached feed entry whose
    ///        version == @p old_version. Only THIS contribution is removed; a
    ///        constraint another still-selected package also declared is retained.
    void withdrawOldContribution(const QString& pkg_id, const QString& old_version);

    /// @brief Withdraw @p contributor from the constraint and demand recorded for
    ///        each of @p deps (the superseded version's declared dependencies).
    void retractContribution(const QString& contributor, const QVector<NuGetDependency>& deps);

    /// @brief Remove @p contributor from the (id, range) constraint's provenance;
    ///        drop the range from the active set only when no contributor remains.
    void withdrawConstraint(const QString& id, const QString& range, const QString& contributor);

    /// @brief Remove @p contributor from @p id's demand provenance; if nothing else
    ///        demands it, drop its still-pending (non-root) fetch so a package no
    ///        package needs is not fetched into the closure.
    void withdrawDemand(const QString& id, const QString& contributor);

    /// @brief Drop @p lower_id from the pending queue + visited set when it is still
    ///        pending and is not a root fetch (a root is never withdrawn).
    void dropPendingUnneeded(const QString& lower_id);

    int m_max_depth;
    int m_max_packages;
    QList<QueueItem> m_queue;
    QSet<QString> m_visited;                         ///< lowercased ids already enqueued
    QHash<QString, QVector<QString>> m_constraints;  ///< lowercased id -> declared ranges
    QVector<ResolvedPackage> m_resolved;
    QStringList m_errors;
    bool m_validated{false};  ///< the final constraint pass has run for this resolution

    /// Cached feed + originating queue item per resolved id, so a late-arriving
    /// constraint can re-run selection without a network refetch.
    QHash<QString, QVector<FeedPackageVersion>> m_feeds;  ///< lowercased id -> feed
    QHash<QString, QueueItem> m_items;                    ///< lowercased id -> resolved item

    /// Per-constraint / per-demand provenance for EXACT retraction on reselection.
    /// Each edge records its declaring package-version contributor; a superseded
    /// version withdraws only its own contribution, so a constraint or queued id
    /// another still-selected package legitimately declared is never dropped
    /// (a blind removal would be a fail-open). Contributor key = "<id>\x1f<version>".
    QHash<QString, QSet<QString>> m_constraint_src;  ///< "<id>\x1f<range>" -> contributors
    QHash<QString, QSet<QString>> m_demand_src;      ///< lowercased id -> demanding contributors
};

}  // namespace sak
