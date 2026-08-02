// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file nuget_dependency_resolver.cpp
/// @brief Implementation of the transport-agnostic transitive dependency resolver.

#include "sak/nuget_dependency_resolver.h"

#include <QDomDocument>

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
    for (const QString& bound : bounds) {
        const auto v = NuGetVersion::parse(bound.trimmed());
        if (v.has_value() && v->isPrerelease()) {
            return true;
        }
    }
    return false;
}

/// @brief True if the id's own range OR any recorded constraint targets prerelease.
bool anyRangeTargetsPrerelease(const QString& own_range, const QVector<QString>& constraints) {
    if (rangeTargetsPrerelease(own_range)) {
        return true;
    }
    for (const QString& range : constraints) {
        if (rangeTargetsPrerelease(range)) {
            return true;
        }
    }
    return false;
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
    if (!ranges.contains(range)) {
        ranges.append(range);
    }
}

bool NuGetDependencyResolver::satisfiesAllConstraints(const QString& id,
                                                      const QString& own_range,
                                                      const NuGetVersion& version) const {
    if (!NuGetVersionRange::parse(own_range).satisfies(version)) {
        return false;
    }
    for (const QString& range : m_constraints.value(id.toLower())) {
        if (!NuGetVersionRange::parse(range).satisfies(version)) {
            return false;
        }
    }
    return true;
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

void NuGetDependencyResolver::provideFeed(const QString& id,
                                          const QVector<FeedPackageVersion>& versions) {
    if (m_queue.isEmpty()) {
        return;
    }
    const QueueItem item = m_queue.takeFirst();
    Q_UNUSED(id);  // the front of the queue is authoritative; id is a caller convenience
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

    ResolvedPackage resolved;
    resolved.package_id = item.id;
    resolved.version = chosen->version;
    resolved.version_range = item.version_range;
    resolved.depth = item.depth;
    for (const NuGetDependency& dep : chosen->dependencies) {
        if (!dep.id.isEmpty()) {
            resolved.dependencies.append(dep.id);
        }
    }
    m_resolved.append(resolved);

    enqueueDependencies(*chosen, item.depth);
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
        const QString lower = dep.id.toLower();
        if (m_visited.contains(lower)) {
            continue;  // already scheduled -- keeps a cyclic graph from looping
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
    const QueueItem item = m_queue.takeFirst();
    Q_UNUSED(id);
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
        const QString lower = id.toLower();
        if (seen.contains(lower)) {
            continue;
        }
        seen.insert(lower);
        NuGetDependency dep;
        dep.id = id;
        dep.version_range = part.section(QLatin1Char(':'), 1, 1).trimmed();
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
