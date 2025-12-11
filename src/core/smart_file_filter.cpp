#include "sak/smart_file_filter.h"
#include <QDir>

namespace sak {

SmartFileFilter::SmartFileFilter(const SmartFilter& rules)
    : m_rules(rules)
{
    compileRegexPatterns();
    
    // Convert lists to sets for faster lookup
    for (const QString& file : m_rules.dangerous_files) {
        m_dangerousFilesSet.insert(file.toLower());
    }
    
    for (const QString& folder : m_rules.exclude_folders) {
        m_excludeFoldersSet.insert(folder.toLower());
    }
}

void SmartFileFilter::setRules(const SmartFilter& rules) {
    m_rules = rules;
    compileRegexPatterns();
    
    m_dangerousFilesSet.clear();
    for (const QString& file : m_rules.dangerous_files) {
        m_dangerousFilesSet.insert(file.toLower());
    }
    
    m_excludeFoldersSet.clear();
    for (const QString& folder : m_rules.exclude_folders) {
        m_excludeFoldersSet.insert(folder.toLower());
    }
}

void SmartFileFilter::compileRegexPatterns() {
    m_compiledPatterns.clear();
    
    for (const QString& pattern : m_rules.exclude_patterns) {
        QRegularExpression regex(pattern, QRegularExpression::CaseInsensitiveOption);
        if (regex.isValid()) {
            m_compiledPatterns.append(regex);
        }
    }
}

bool SmartFileFilter::shouldExcludeFile(const QFileInfo& fileInfo, const QString& profilePath) const {
    QString fileName = fileInfo.fileName();
    QString absolutePath = fileInfo.absoluteFilePath();
    
    // 1. Check if it's a dangerous file (NTUSER.DAT, etc.)
    if (isDangerousFile(fileName)) {
        return true;
    }
    
    // 2. Check file size
    if (exceedsSizeLimit(fileInfo.size())) {
        return true;
    }
    
    // 3. Check filename patterns (*.tmp, *.lock, etc.)
    if (matchesPattern(fileName)) {
        return true;
    }
    
    // 4. Check if in excluded folder (Cache, Temp, etc.)
    QString relativePath = QDir(profilePath).relativeFilePath(absolutePath);
    if (isInExcludedFolder(relativePath)) {
        return true;
    }
    
    // 5. Check if in cache directory
    if (isInCacheDirectory(absolutePath)) {
        return true;
    }
    
    return false;
}

bool SmartFileFilter::shouldExcludeFolder(const QFileInfo& folderInfo, const QString& profilePath) const {
    QString folderName = folderInfo.fileName();
    QString absolutePath = folderInfo.absoluteFilePath();
    
    // Check if folder name is in exclusion list
    if (m_excludeFoldersSet.contains(folderName.toLower())) {
        return true;
    }
    
    // Check relative path for excluded folders
    QString relativePath = QDir(profilePath).relativeFilePath(absolutePath);
    if (isInExcludedFolder(relativePath)) {
        return true;
    }
    
    // Exclude cache directories
    if (isInCacheDirectory(absolutePath)) {
        return true;
    }
    
    return false;
}

bool SmartFileFilter::exceedsSizeLimit(qint64 size) const {
    // Only enforce limit if enabled
    if (!m_rules.enable_file_size_limit) {
        return false;
    }
    return size > m_rules.max_single_file_size;
}

bool SmartFileFilter::isDangerousFile(const QString& fileName) const {
    return m_dangerousFilesSet.contains(fileName.toLower());
}

bool SmartFileFilter::matchesPattern(const QString& fileName) const {
    for (const QRegularExpression& regex : m_compiledPatterns) {
        if (regex.match(fileName).hasMatch()) {
            return true;
        }
    }
    return false;
}

bool SmartFileFilter::isInExcludedFolder(const QString& relativePath) const {
    // Split path into components
    QStringList components = relativePath.split('/', Qt::SkipEmptyParts);
    components.append(relativePath.split('\\', Qt::SkipEmptyParts));
    
    for (const QString& component : components) {
        if (m_excludeFoldersSet.contains(component.toLower())) {
            return true;
        }
    }
    
    return false;
}

bool SmartFileFilter::isInCacheDirectory(const QString& path) const {
    QString lowerPath = path.toLower();
    
    // Common cache directory patterns
    QStringList cachePatterns = {
        "\\cache\\",
        "\\gpucache\\",
        "\\code cache\\",
        "\\shadercache\\",
        "\\webcache\\",
        "\\service worker\\",
        "\\session storage\\",
        "/cache/",
        "/gpucache/",
        "/code cache/",
        "/shadercache/",
        "/webcache/",
        "/service worker/",
        "/session storage/"
    };
    
    for (const QString& pattern : cachePatterns) {
        if (lowerPath.contains(pattern)) {
            return true;
        }
    }
    
    return false;
}

QString SmartFileFilter::getExclusionReason(const QFileInfo& fileInfo) const {
    QString fileName = fileInfo.fileName();
    
    if (isDangerousFile(fileName)) {
        return QString("Dangerous system file: %1 (would corrupt profile)").arg(fileName);
    }
    
    if (exceedsSizeLimit(fileInfo.size())) {
        double sizeMB = fileInfo.size() / (1024.0 * 1024.0);
        if (m_rules.enable_file_size_limit) {
            return QString("File too large: %1 MB (limit: %2 MB)")
                .arg(sizeMB, 0, 'f', 1)
                .arg(m_rules.max_single_file_size / (1024.0 * 1024.0), 0, 'f', 0);
        }
    }
    
    if (matchesPattern(fileName)) {
        return QString("Matches exclusion pattern: %1").arg(fileName);
    }
    
    if (isInCacheDirectory(fileInfo.absoluteFilePath())) {
        return "Located in cache directory";
    }
    
    return "Excluded by filter rules";
}

} // namespace sak
