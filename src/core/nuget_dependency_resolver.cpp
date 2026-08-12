// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file nuget_dependency_resolver.cpp
/// @brief Implementation of the transport-agnostic transitive dependency resolver.

#include "sak/nuget_dependency_resolver.h"

#include <QDomDocument>

#include <algorithm>

namespace sak {

namespace {

/// @brief Read a property from an OData <m:properties>/<properties> element,
///        trying the d: namespace prefix first (standard OData) then plain.
QString extractProperty(const QDomElement& properties, const QString& name) {
    QDomElement elem = properties.firstChildElement("d:" + name);
    if (elem.isNull()) {
        elem = properties.firstChildElement(name);
    }
    return elem.isNull() ? QString() : elem.text().trimmed();
}

/// @brief Locate the properties element of an OData <entry>.
QDomElement findProperties(const QDomElement& entry) {
    QDomNodeList nodes = entry.elementsByTagName("m:properties");
    if (nodes.isEmpty()) {
        nodes = entry.elementsByTagName("properties");
    }
    return nodes.isEmpty() ? QDomElement() : nodes.at(0).toElement();
}

/// @brief True if @p candidate should replace @p best as the selected version.
///        Higher precedence wins; at equal precedence a stable release is
///        preferred over a prerelease.
bool isBetterCandidate(const NuGetVersion& candidate, const std::optional<NuGetVersion>& best) {
    if (!best.has_value()) {
        return true;
    }
    const int cmp = candidate.compare(*best);
    if (cmp != 0) {
        return cmp > 0;
    }
    return best->isPrerelease() && !candidate.isPrerelease();
}

/// @brief Find the feed entry whose version equals an exactly-pinned root
///        version. std::nullopt if the pin is unparseable or unavailable.
std::optional<FeedPackageVersion> selectPinnedVersion(const QString& pinned,
                                                      const QVector<FeedPackageVersion>& versions) {
    const auto wanted = NuGetVersion::parse(pinned);
    if (!wanted.has_value()) {
        return std::nullopt;
    }
    for (const FeedPackageVersion& fv : versions) {
        const auto parsed = NuGetVersion::parse(fv.version);
        if (parsed.has_value() && parsed->compare(*wanted) == 0) {
            return fv;
        }
    }
    return std::nullopt;
}

/// @brief True if a version range explicitly targets a prerelease (any of its
///        bounds is itself a prerelease version). NuGet only considers prerelease
///        candidates when the range opts in this way; otherwise a stable release
///        must be chosen even if a higher prerelease exists (e.g. 1.9.0 wins over
///        2.0.0-beta for "[1.0,)").
bool rangeTargetsPrerelease(const QString& range) {
    QString inner = range;
    inner.remove(QLatin1Char('['))
        .remove(QLatin1Char(']'))
        .remove(QLatin1Char('('))
        .remove(QLatin1Char(')'));
    const QStringList bounds = inner.split(QLatin1Char(','));
    return std::ranges::any_of(bounds, [](const QString& bound) {
        const auto v = NuGetVersion::parse(bound.trimmed());
        return v.has_value() && v->isPrerelease();
    });
}

/// @brief True if the id's own range OR any recorded constraint targets prerelease.
bool anyRangeTargetsPrerelease(const QString& own_range, const QVector<QString>& constraints) {
    if (rangeTargetsPrerelease(own_range)) {
        return true;
    }
    return std::ranges::any_of(constraints, rangeTargetsPrerelease);
}

}  // namespace

NuGetDependencyResolver::NuGetDependencyResolver(int max_depth, int max_packages)
    : m_max_depth(max_depth), m_max_packages(max_packages) {}

void NuGetDependencyResolver::start(const QString& root_id, const QString& root_version) {
    m_queue.clear();
    m_visited.clear();
    m_constraints.clear();
    m_resolved.clear();
    m_errors.clear();
    m_feeds.clear();
    m_items.clear();
    m_validated = false;
    addRoot(root_id, root_version);
}

void NuGetDependencyResolver::addRoot(const QString& root_id, const QString& root_version) {
    if (root_id.isEmpty()) {
        m_errors.append(QStringLiteral("Empty root package id"));
        return;
    }
    const QString lower = root_id.toLower();
    if (m_visited.contains(lower)) {
        // Already scheduled (duplicate root, or already pulled in as a dependency).
        // A second root that pins a DIFFERENT version cannot also be bundled (one
        // version per id is scheduled), so surface the dropped pin instead of
        // silently discarding it.
        if (!root_version.isEmpty()) {
            const QString range = QStringLiteral("[%1]").arg(root_version);
            if (!m_constraints.value(lower).contains(range)) {
                m_errors.append(
                    QStringLiteral("Package %1 requested at multiple versions; keeping the first "
                                   "and ignoring the additional pin %2")
                        .arg(root_id, root_version));
            }
        }
        return;
    }
    m_visited.insert(lower);
    // A root added after a previous drain re-opens resolution. m_validated latches
    // true the first time the queue empties, which would otherwise let the final
    // constraint pass be skipped for this new root's subtree; clear it so
    // maybeValidateOnDrain() runs again when the queue drains next.
    m_validated = false;

    QueueItem root;
    root.id = root_id;
    root.version_range = root_version.isEmpty() ? QString()
                                                : QStringLiteral("[%1]").arg(root_version);
    root.depth = 0;
    root.is_root = true;
    root.root_version = root_version;
    m_queue.append(root);
    recordConstraint(root_id, root.version_range);
}

void NuGetDependencyResolver::recordConstraint(const QString& id, const QString& range) {
    QVector<QString>& ranges = m_constraints[id.toLower()];
    if (ranges.contains(range)) {
        return;  // this exact (id, range) constraint is already recorded
    }
    // A non-empty range we cannot parse is MALFORMED (an empty range is the permissive
    // "any"). Keep it recorded so selection stays fail-closed -- a malformed range
    // satisfies no version -- but surface it as a distinct resolution warning so it is
    // not silently mistaken for an unconstrained edge (P7-46).
    if (!range.isEmpty() && !NuGetVersionRange::parse(range).isValid()) {
        m_errors.append(
            QStringLiteral("Malformed version range '%1' declared for %2; no version can satisfy "
                           "it (fail-closed)")
                .arg(range, id));
    }
    ranges.append(range);
}

bool NuGetDependencyResolver::satisfiesAllConstraints(const QString& id,
                                                      const QString& own_range,
                                                      const NuGetVersion& version) const {
    if (!NuGetVersionRange::parse(own_range).satisfies(version)) {
        return false;
    }
    const QVector<QString> ranges = m_constraints.value(id.toLower());
    return std::ranges::all_of(ranges, [&version](const QString& range) {
        return NuGetVersionRange::parse(range).satisfies(version);
    });
}

void NuGetDependencyResolver::validateConstraints() {
    for (const ResolvedPackage& pkg : m_resolved) {
        const auto parsed = NuGetVersion::parse(pkg.version);
        if (!parsed.has_value()) {
            continue;
        }
        for (const QString& range : m_constraints.value(pkg.package_id.toLower())) {
            if (!NuGetVersionRange::parse(range).satisfies(*parsed)) {
                m_errors.append(
                    QStringLiteral("Version conflict: selected %1 %2 does not satisfy a required "
                                   "range '%3' declared by another package")
                        .arg(pkg.package_id, pkg.version, range));
            }
        }
    }
}

QString NuGetDependencyResolver::nextFetchId() const {
    return m_queue.isEmpty() ? QString() : m_queue.first().id;
}

QString NuGetDependencyResolver::nextFetchRange() const {
    return m_queue.isEmpty() ? QString() : m_queue.first().version_range;
}

std::optional<FeedPackageVersion> NuGetDependencyResolver::selectVersion(
    const QueueItem& item, const QVector<FeedPackageVersion>& versions) const {
    // A root pinned to an exact version must match it exactly -- never silently
    // substitute a different version.
    if (item.is_root && !item.root_version.isEmpty()) {
        return selectPinnedVersion(item.root_version, versions);
    }

    // A prerelease candidate is only eligible when a relevant range explicitly
    // targets prerelease (NuGet's default excludes prerelease), so a higher
    // prerelease never shadows a satisfying stable release.
    const bool allow_prerelease = anyRangeTargetsPrerelease(item.version_range,
                                                            m_constraints.value(item.id.toLower()));

    // Honor EVERY declared range for this id (a diamond may constrain it from two
    // edges), picking the highest version that satisfies them all.
    std::optional<NuGetVersion> best;
    std::optional<FeedPackageVersion> chosen;
    for (const FeedPackageVersion& fv : versions) {
        const auto parsed = NuGetVersion::parse(fv.version);
        if (!parsed.has_value() || (parsed->isPrerelease() && !allow_prerelease) ||
            !satisfiesAllConstraints(item.id, item.version_range, *parsed)) {
            continue;
        }
        if (isBetterCandidate(*parsed, best)) {
            best = parsed;
            chosen = fv;
        }
    }
    return chosen;
}

int NuGetDependencyResolver::pendingIndexForId(const QString& id) const {
    for (int i = 0; i < m_queue.size(); ++i) {
        if (m_queue.at(i).id.compare(id, Qt::CaseInsensitive) == 0) {
            return i;
        }
    }
    return -1;
}

void NuGetDependencyResolver::provideFeed(const QString& id,
                                          const QVector<FeedPackageVersion>& versions) {
    if (m_queue.isEmpty()) {
        return;
    }
    // Bind the feed to the item it NAMES, not to the queue head: applying a
    // response for one id onto a different pending id would resolve a package to
    // another package's versions (a wrong/hostile substitution). A response that
    // matches nothing pending is dropped, not misapplied.
    const int idx = pendingIndexForId(id);
    if (idx < 0) {
        m_errors.append(
            QStringLiteral("Ignored feed response for '%1': no pending fetch has that id").arg(id));
        return;
    }
    const QueueItem item = m_queue.takeAt(idx);
    resolveDequeued(item, versions);
    maybeValidateOnDrain();
}

void NuGetDependencyResolver::resolveDequeued(const QueueItem& item,
                                              const QVector<FeedPackageVersion>& versions) {
    const auto chosen = selectVersion(item, versions);
    if (!chosen.has_value()) {
        m_errors.append(
            QStringLiteral("No available version of %1 satisfies range '%2'")
                .arg(item.id,
                     item.version_range.isEmpty() ? QStringLiteral("(any)") : item.version_range));
        return;
    }
    if (m_resolved.size() >= m_max_packages) {
        m_errors.append(QStringLiteral("Dependency closure exceeded %1 packages; %2 dropped")
                            .arg(m_max_packages)
                            .arg(item.id));
        return;
    }

    ResolvedPackage resolved_pkg;
    resolved_pkg.package_id = item.id;
    resolved_pkg.version = chosen->version;
    resolved_pkg.version_range = item.version_range;
    resolved_pkg.depth = item.depth;
    for (const NuGetDependency& dep : chosen->dependencies) {
        if (!dep.id.isEmpty()) {
            resolved_pkg.dependencies.append(dep.id);
        }
    }
    m_resolved.append(resolved_pkg);

    // Cache this package's feed + item so a constraint discovered later (a diamond
    // edge) can re-run selection against it without a network refetch.
    const QString lower = item.id.toLower();
    m_feeds.insert(lower, versions);
    m_items.insert(lower, item);

    enqueueDependencies(*chosen, item.depth);
}

int NuGetDependencyResolver::resolvedIndex(const QString& lower_id) const {
    for (int i = 0; i < m_resolved.size(); ++i) {
        if (m_resolved.at(i).package_id.toLower() == lower_id) {
            return i;
        }
    }
    return -1;
}

void NuGetDependencyResolver::reselectIfResolved(const QString& id) {
    const QString lower = id.toLower();
    if (!m_feeds.contains(lower)) {
        return;  // not resolved yet: its own selection will honor the constraint
    }
    const int idx = resolvedIndex(lower);
    if (idx < 0) {
        return;
    }
    const QString range = m_resolved.at(idx).version_range;
    const auto current = NuGetVersion::parse(m_resolved.at(idx).version);
    if (current.has_value() && satisfiesAllConstraints(id, range, *current)) {
        return;  // the current pick still satisfies every recorded constraint
    }
    const auto chosen = selectVersion(m_items.value(lower), m_feeds.value(lower));
    if (chosen.has_value() && chosen->version != m_resolved.at(idx).version) {
        applyReselection(idx, *chosen);
    }
    // If nothing new satisfies, the hard conflict is left for validateConstraints().
}

void NuGetDependencyResolver::applyReselection(int idx, const FeedPackageVersion& chosen) {
    m_resolved[idx].version = chosen.version;
    m_resolved[idx].dependencies.clear();
    for (const NuGetDependency& dep : chosen.dependencies) {
        if (!dep.id.isEmpty()) {
            m_resolved[idx].dependencies.append(dep.id);
        }
    }
    enqueueDependencies(chosen, m_resolved[idx].depth);
}

void NuGetDependencyResolver::maybeValidateOnDrain() {
    if (m_queue.isEmpty() && !m_validated) {
        m_validated = true;
        validateConstraints();  // catch any constraint discovered after selection
    }
}

void NuGetDependencyResolver::enqueueDependencies(const FeedPackageVersion& selected, int depth) {
    if (selected.dependencies.isEmpty()) {
        return;
    }
    // Cap recursion depth (root = 0). Deeper edges are dropped but surfaced.
    if (depth + 1 >= m_max_depth) {
        m_errors.append(QStringLiteral("Dependency depth cap (%1) reached; deeper deps dropped")
                            .arg(m_max_depth));
        return;
    }
    for (const NuGetDependency& dep : selected.dependencies) {
        if (dep.id.isEmpty()) {
            continue;
        }
        // Record the constraint for EVERY edge, even one whose id is already
        // scheduled (a diamond's second edge) so it is not silently discarded.
        recordConstraint(dep.id, dep.version_range);
        // If that id is ALREADY resolved, its earlier selection did not see this
        // edge's range; re-select over its cached feed so a resolvable diamond is
        // corrected now instead of only erroring at the final validation pass.
        reselectIfResolved(dep.id);
        const QString lower = dep.id.toLower();
        if (m_visited.contains(lower)) {
            continue;  // already scheduled -- keeps a cyclic graph from looping
        }
        // Bound total scheduled work: never let resolved + pending exceed the package
        // cap. A hostile or oversized feed can declare an enormous dependency fan-out,
        // and without this the queue (and m_visited) would grow unbounded before the
        // resolve-time cap ever fired. Deeper edges beyond the cap are surfaced, not
        // silently dropped. Ids past the cap could never resolve anyway (the resolve
        // cap drops them), so a fully resolvable closure is never truncated here.
        if (m_resolved.size() + m_queue.size() >= m_max_packages) {
            m_errors.append(
                QStringLiteral("Dependency closure exceeded %1 packages; dropped %2 and later deps")
                    .arg(m_max_packages)
                    .arg(dep.id));
            break;
        }
        m_visited.insert(lower);
        QueueItem child;
        child.id = dep.id;
        child.version_range = dep.version_range;
        child.depth = depth + 1;
        m_queue.append(child);
    }
}

void NuGetDependencyResolver::provideFeedFailure(const QString& id) {
    if (m_queue.isEmpty()) {
        return;
    }
    const int idx = pendingIndexForId(id);
    if (idx < 0) {
        m_errors.append(
            QStringLiteral("Ignored feed failure for '%1': no pending fetch has that id").arg(id));
        return;
    }
    const QueueItem item = m_queue.takeAt(idx);
    m_errors.append(QStringLiteral("Failed to fetch dependency feed for %1").arg(item.id));
    maybeValidateOnDrain();
}

void NuGetDependencyResolver::cancel() {
    m_queue.clear();
}

QVector<NuGetDependency> NuGetDependencyResolver::parseDependencies(const QString& dep_string) {
    // NuGet v2 OData "Dependencies": pipe-separated "id:versionRange:targetFramework".
    // Version ranges use interval notation ("[1.0, 2.0)") and contain no colons, so
    // the range is always the second colon field. Framework-only markers ("::net45")
    // have an empty id and are skipped.
    QVector<NuGetDependency> result;
    QSet<QString> seen;
    const QStringList parts = dep_string.split(QLatin1Char('|'), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        const QString id = part.section(QLatin1Char(':'), 0, 0).trimmed();
        if (id.isEmpty()) {
            continue;
        }
        const QString range = part.section(QLatin1Char(':'), 1, 1).trimmed();
        // Dedup on the (id, range) PAIR, not id alone: the same id can recur with
        // a DIFFERENT range per target framework. Dropping the later entry by id
        // would silently discard a distinct constraint (a needed floor/ceiling),
        // so every distinct range is kept and later intersected during selection.
        const QString key = id.toLower() + QLatin1Char('\x1f') + range;
        if (seen.contains(key)) {
            continue;
        }
        seen.insert(key);
        NuGetDependency dep;
        dep.id = id;
        dep.version_range = range;
        result.append(dep);
    }
    return result;
}

QVector<FeedPackageVersion> NuGetDependencyResolver::parseODataFeedVersions(const QByteArray& xml) {
    QVector<FeedPackageVersion> out;
    QDomDocument doc;
    if (!doc.setContent(xml)) {
        return out;
    }
    const QDomNodeList entries = doc.documentElement().elementsByTagName(QStringLiteral("entry"));
    for (int i = 0; i < entries.count(); ++i) {
        const QDomElement entry = entries.at(i).toElement();
        if (entry.isNull()) {
            continue;
        }
        const QDomElement properties = findProperties(entry);
        if (properties.isNull()) {
            continue;
        }
        FeedPackageVersion fv;
        fv.version = extractProperty(properties, QStringLiteral("Version"));
        if (fv.version.isEmpty()) {
            continue;
        }
        const QString deps = extractProperty(properties, QStringLiteral("Dependencies"));
        if (!deps.isEmpty()) {
            fv.dependencies = parseDependencies(deps);
        }
        out.append(fv);
    }
    return out;
}

}  // namespace sak
