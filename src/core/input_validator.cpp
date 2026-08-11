// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file input_validator.cpp
/// @brief Implementation of input validation utilities

#include "sak/input_validator.h"

#include "sak/logger.h"
#include "sak/path_utils.h"

#include <QtGlobal>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <regex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <psapi.h>
// Undefine Windows macros that conflict with Qt
#undef emit
#undef signals
#undef slots
#else
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

namespace sak {

namespace {

constexpr unsigned char kContinuationMask = 0xC0;
constexpr unsigned char kContinuationTag = 0x80;
constexpr unsigned char kAsciiLimit = 0x80;
constexpr unsigned char kUtf8TwoByteMask = 0xE0;
constexpr unsigned char kUtf8TwoByteTag = 0xC0;
constexpr unsigned char kUtf8TwoByteMinLead = 0xC2;
constexpr unsigned char kUtf8ThreeByteMask = 0xF0;
constexpr unsigned char kUtf8ThreeByteTag = 0xE0;
constexpr unsigned char kUtf8FourByteMask = 0xF8;
constexpr unsigned char kUtf8FourByteTag = 0xF0;
constexpr int kUtf8OneContinuation = 1;
constexpr int kUtf8TwoContinuations = 2;
constexpr int kUtf8ThreeContinuations = 3;
constexpr int kUtf8TwoByteSequenceLength = 2;
constexpr int kUtf8ThreeByteSequenceLength = 3;
constexpr int kUtf8FourByteSequenceLength = 4;
// RFC 3629 second-byte boundaries used to reject overlong encodings, UTF-16
// surrogates (U+D800..U+DFFF), and code points above U+10FFFF.
constexpr unsigned char kUtf8ContinuationMin = 0x80;
constexpr unsigned char kUtf8ContinuationMax = 0xBF;
constexpr unsigned char kUtf8ThreeByteLeadE0 = 0xE0;
constexpr unsigned char kUtf8ThreeByteLeadED = 0xED;
constexpr unsigned char kUtf8ThreeByteE0MinSecond = 0xA0;  // reject E0 80..9F (overlong)
constexpr unsigned char kUtf8ThreeByteEDMaxSecond = 0x9F;  // reject ED A0..BF (surrogate)
constexpr unsigned char kUtf8FourByteMinLead = 0xF0;
constexpr unsigned char kUtf8FourByteMaxLead = 0xF4;       // U+10FFFF cap
constexpr unsigned char kUtf8FourByteLeadF4 = 0xF4;
constexpr unsigned char kUtf8FourByteF0MinSecond = 0x90;   // reject F0 80..8F (overlong)
constexpr unsigned char kUtf8FourByteF4MaxSecond = 0x8F;   // reject F4 90..BF (> U+10FFFF)
constexpr std::uintmax_t kFileDescriptorWarnNumerator = 4;
constexpr std::uintmax_t kFileDescriptorWarnDenominator = 5;
constexpr std::size_t kUnknownHardwareThreadLimit = 64;
constexpr unsigned int kThreadWarningMultiplier = 2;
constexpr unsigned int kThreadRejectMultiplier = 4;
constexpr auto kWindowsDeviceNamePattern = "(^|[/\\\\])(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\\.|$)";

bool isContinuationByte(unsigned char byte) noexcept {
    return (byte & kContinuationMask) == kContinuationTag;
}

bool hasContinuationBytes(std::string_view str, std::size_t pos, int count) noexcept {
    if (pos + static_cast<std::size_t>(count) >= str.length()) {
        return false;
    }
    for (int offset = 1; offset <= count; ++offset) {
        if (!isContinuationByte(static_cast<unsigned char>(str[pos + offset]))) {
            return false;
        }
    }
    return true;
}

// Validate the second byte of a 3-byte sequence per RFC 3629, rejecting
// overlong forms (lead E0, second < 0xA0) and surrogates (lead ED, second > 0x9F).
bool isValidThreeByteSecond(unsigned char lead, unsigned char second) noexcept {
    unsigned char lo = kUtf8ContinuationMin;
    unsigned char hi = kUtf8ContinuationMax;
    if (lead == kUtf8ThreeByteLeadE0) {
        lo = kUtf8ThreeByteE0MinSecond;
    } else if (lead == kUtf8ThreeByteLeadED) {
        hi = kUtf8ThreeByteEDMaxSecond;
    }
    return second >= lo && second <= hi;
}

// Validate the lead and second byte of a 4-byte sequence per RFC 3629, keeping
// only U+10000..U+10FFFF (leads F0..F4 with the matching second-byte window).
bool isValidFourByteLeadPair(unsigned char lead, unsigned char second) noexcept {
    if (lead < kUtf8FourByteMinLead || lead > kUtf8FourByteMaxLead) {
        return false;
    }
    unsigned char lo = kUtf8ContinuationMin;
    unsigned char hi = kUtf8ContinuationMax;
    if (lead == kUtf8FourByteMinLead) {
        lo = kUtf8FourByteF0MinSecond;
    } else if (lead == kUtf8FourByteLeadF4) {
        hi = kUtf8FourByteF4MaxSecond;
    }
    return second >= lo && second <= hi;
}

int checkMultiByteUtf8(std::string_view str, std::size_t pos) noexcept {
    // isValidUtf8() is the only caller and reaches this from inside its
    // `while (i < str.length())` loop, so str is non-empty and pos is in range.
    Q_ASSERT(!str.empty());
    Q_ASSERT(pos < str.size());
    const unsigned char lead = static_cast<unsigned char>(str[pos]);

    if ((lead & kUtf8TwoByteMask) == kUtf8TwoByteTag) {
        return (lead >= kUtf8TwoByteMinLead && hasContinuationBytes(str, pos, kUtf8OneContinuation))
                   ? kUtf8TwoByteSequenceLength
                   : 0;
    }
    if ((lead & kUtf8ThreeByteMask) == kUtf8ThreeByteTag) {
        if (!hasContinuationBytes(str, pos, kUtf8TwoContinuations)) {
            return 0;
        }
        const auto second = static_cast<unsigned char>(str[pos + 1]);
        return isValidThreeByteSecond(lead, second) ? kUtf8ThreeByteSequenceLength : 0;
    }
    if ((lead & kUtf8FourByteMask) == kUtf8FourByteTag) {
        if (!hasContinuationBytes(str, pos, kUtf8ThreeContinuations)) {
            return 0;
        }
        const auto second = static_cast<unsigned char>(str[pos + 1]);
        return isValidFourByteLeadPair(lead, second) ? kUtf8FourByteSequenceLength : 0;
    }
    return 0;
}

bool isPrintableString(std::string_view str) {
    return std::all_of(str.begin(), str.end(), [](unsigned char c) { return std::isprint(c); });
}

bool isAsciiString(std::string_view str) {
    return std::all_of(str.begin(), str.end(), [](unsigned char c) { return c < kAsciiLimit; });
}

validation_result validateStringLengthPolicy(std::string_view str,
                                             const string_validation_config& config) {
    if (str.length() < config.min_length) {
        return input_validator::failure(error_code::validation_failed, "String is too short");
    }
    if (str.length() > config.max_length) {
        return input_validator::failure(error_code::validation_failed, "String is too long");
    }
    return input_validator::success();
}

validation_result validateStringCharacterPolicy(std::string_view str,
                                                const string_validation_config& config) {
    if (!config.allow_null_bytes && input_validator::containsNullBytes(str)) {
        return input_validator::failure(error_code::validation_failed,
                                        "String contains null bytes");
    }
    if (!config.allow_control_chars && input_validator::containsControlChars(str)) {
        return input_validator::failure(error_code::validation_failed,
                                        "String contains control characters");
    }
    if (config.require_printable && !isPrintableString(str)) {
        return input_validator::failure(error_code::validation_failed,
                                        "String contains non-printable characters");
    }
    return input_validator::success();
}

validation_result validateStringEncodingPolicy(std::string_view str,
                                               const string_validation_config& config) {
    if (config.require_ascii && !isAsciiString(str)) {
        return input_validator::failure(error_code::validation_failed,
                                        "String contains non-ASCII characters");
    }
    if (config.require_utf8 && !input_validator::isValidUtf8(str)) {
        return input_validator::failure(error_code::validation_failed, "String is not valid UTF-8");
    }
    return input_validator::success();
}

}  // anonymous namespace

// ============================================
// Path Validation
// ============================================

namespace {

#ifdef _WIN32
bool componentIsReparsePoint(const std::filesystem::path& p) {
    // Catches BOTH symlinks and junctions (mount points): std::filesystem's
    // is_symlink does not report a junction, but the reparse-point attribute
    // does, and a junction ancestor is exactly the traversal we must block.
    const DWORD attrs = GetFileAttributesW(p.wstring().c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    }
    // A component that is genuinely ABSENT (a not-yet-created leaf/ancestor) is
    // safely not a reparse point. Any OTHER failure (access denied, sharing
    // violation, device not ready) leaves the component's nature UNKNOWN and must
    // fail CLOSED: report it as a blocking reparse point rather than silently treat
    // it as a plain, traversable path.
    const DWORD err = GetLastError();
    return err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND;
}
#else
bool componentIsReparsePoint(const std::filesystem::path& p) {
    std::error_code ec;
    const bool is_link = std::filesystem::is_symlink(p, ec);
    if (!ec) {
        return is_link;
    }
    // A genuinely-absent component is not a link; any OTHER status error leaves its
    // nature unknown and must fail CLOSED (treat as a blocking link) rather than be
    // silently read as "not a link".
    return ec != std::errc::no_such_file_or_directory;
}
#endif

// True if ANY component along the path (any ancestor, not just the final one) is
// a reparse point. Non-existent components simply are not reparse points, so this
// is safe to call on a not-yet-created leaf.
bool anyComponentIsReparsePoint(const std::filesystem::path& path) {
    for (const auto& prefix : input_validator::pathPrefixes(path)) {
        if (componentIsReparsePoint(prefix)) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::vector<std::filesystem::path> input_validator::pathPrefixes(
    const std::filesystem::path& path) {
    std::vector<std::filesystem::path> prefixes;
    std::filesystem::path current;
    for (const auto& part : path) {
        current /= part;
        prefixes.push_back(current);
    }
    return prefixes;
}

namespace {

// Enforce the file/directory type constraints for an existing path. Split out so
// validatePathExistence stays within the complexity budget.
validation_result validatePathTypeConstraints(const std::filesystem::path& path,
                                              const path_validation_config& config) {
    std::error_code ec;
    if (config.must_be_directory && !std::filesystem::is_directory(path, ec)) {
        return input_validator::failure(error_code::not_a_directory, "Path must be a directory");
    }
    if (config.must_be_file && !std::filesystem::is_regular_file(path, ec)) {
        return input_validator::failure(error_code::invalid_file, "Path must be a regular file");
    }
    return input_validator::success();
}

}  // namespace

validation_result input_validator::validatePathExistence(const std::filesystem::path& path,
                                                         const path_validation_config& config) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) {
        // A stat that genuinely FAILS (permission denied on an ancestor, an I/O
        // error) is not the same as a path that is simply absent: exists() clears
        // ec for a genuinely-absent path and sets it only on a real error. Fail
        // CLOSED rather than let an optional (must_exist == false) path pass on a
        // status we could not resolve.
        return failure(error_code::filesystem_error, "Cannot determine whether path exists");
    }

    if (config.must_exist && !exists) {
        return failure(error_code::file_not_found, "Path must exist but does not");
    }

    // Reject a symlink/junction ANYWHERE in the path when links are disallowed --
    // not just the final component. An ancestor junction would otherwise redirect
    // the whole subtree past this check. Run this even for a not-yet-existing leaf
    // so a later write cannot be smuggled through an existing ancestor reparse.
    if (!config.allow_symlinks && anyComponentIsReparsePoint(path)) {
        return failure(error_code::invalid_path,
                       "Symbolic links / reparse points are not allowed in the path");
    }

    if (!exists) {
        return success();
    }

    return validatePathTypeConstraints(path, config);
}

namespace {

#ifdef _WIN32
// Probe whether opening @p path for @p desired_access is denied by the file's
// DACL. Only ERROR_ACCESS_DENIED counts as a denial; other open failures
// (sharing violations, transient errors) are not treated as permission
// problems here so the advisory check does not produce false positives.
bool windowsAccessDenied(const std::filesystem::path& path, DWORD desired_access, DWORD attrs) {
    const DWORD flags = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0 ? FILE_FLAG_BACKUP_SEMANTICS : 0UL;
    HANDLE handle = CreateFileW(path.wstring().c_str(),
                                desired_access,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr,
                                OPEN_EXISTING,
                                flags,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    CloseHandle(handle);
    return false;
}

validation_result checkWindowsPermissions(const std::filesystem::path& path,
                                          const path_validation_config& config) {
    DWORD const attrs = GetFileAttributesW(path.wstring().c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        if (config.check_read_permission || config.check_write_permission) {
            return input_validator::failure(error_code::permission_denied,
                                            "Cannot check file permissions");
        }
        return input_validator::success();
    }

    // Actually test read access against the DACL rather than assuming a file
    // with normal attributes is readable (a deny-read ACE leaves attributes
    // untouched but still blocks the open).
    if (config.check_read_permission && windowsAccessDenied(path, GENERIC_READ, attrs)) {
        return input_validator::failure(error_code::permission_denied, "Path is not readable");
    }

    if (config.check_write_permission) {
        if ((attrs & FILE_ATTRIBUTE_READONLY) != 0) {
            return input_validator::failure(error_code::permission_denied, "Path is read-only");
        }
        // Directories cannot be opened for GENERIC_WRITE data access, so limit
        // the ACL-based write probe to regular files.
        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
            windowsAccessDenied(path, GENERIC_WRITE, attrs)) {
            return input_validator::failure(error_code::permission_denied, "Path is not writable");
        }
    }

    return input_validator::success();
}
#else
validation_result checkUnixPermissions(const std::filesystem::path& path,
                                       const path_validation_config& config) {
    if (config.check_read_permission && access(path.c_str(), R_OK) != 0) {
        return input_validator::failure(error_code::permission_denied, "Path is not readable");
    }

    if (config.check_write_permission && access(path.c_str(), W_OK) != 0) {
        return input_validator::failure(error_code::permission_denied, "Path is not writable");
    }

    return input_validator::success();
}
#endif

// Split a path into normalized, comparable components. On Windows the components
// are lowercased so containment is case-insensitive (matching the platform
// filesystem). Empty (trailing-separator) and "." elements are dropped so
// "C:/safe/" and "C:/safe" compare equal.
std::vector<std::string> normalizedPathComponents(const std::filesystem::path& p) {
    std::vector<std::string> components;
    for (const auto& part : p.lexically_normal()) {
        std::string component = part.string();
        if (component.empty() || component == ".") {
            continue;
        }
#ifdef _WIN32
        std::transform(component.begin(), component.end(), component.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
#endif
        components.push_back(std::move(component));
    }
    return components;
}

// True when path is base itself or a descendant, compared component-by-component.
// Unlike a raw string prefix test this cannot be fooled by sibling names that
// share a textual prefix ("safe" vs "safeevil").
bool baseContainsPath(const std::filesystem::path& base, const std::filesystem::path& path) {
    const auto base_components = normalizedPathComponents(base);
    const auto path_components = normalizedPathComponents(path);
    if (path_components.size() < base_components.size()) {
        return false;
    }
    return std::equal(base_components.begin(), base_components.end(), path_components.begin());
}

}  // namespace

validation_result input_validator::validatePathPermissions(const std::filesystem::path& path,
                                                           const path_validation_config& config) {
#ifdef _WIN32
    return checkWindowsPermissions(path, config);
#else
    return checkUnixPermissions(path, config);
#endif
}

validation_result input_validator::validatePathSyntax(const std::filesystem::path& path,
                                                      const path_validation_config& config) {
    const auto path_str = path.string();
    if (path_str.length() > config.max_path_length) {
        return failure(error_code::path_too_long, "Path exceeds maximum allowed length");
    }
    if (path_str.find('\0') != std::string::npos) {
        return failure(error_code::invalid_path, "Path contains null bytes");
    }
    if (containsSuspiciousPatterns(path)) {
        return failure(error_code::invalid_path, "Path contains suspicious patterns");
    }
    if (containsTraversalSequences(path)) {
        return failure(error_code::path_traversal_attempt,
                       "Path contains directory traversal sequences");
    }
    if (!config.allow_relative_paths && path.is_relative()) {
        return failure(error_code::invalid_path, "Relative paths are not allowed");
    }
    return success();
}

validation_result input_validator::validatePath(const std::filesystem::path& path,
                                                const path_validation_config& config) {
    // Fail closed on an empty path: a blank target is not a valid path and must never
    // pass validation, where a later step could resolve it to the current directory.
    if (path.empty()) {
        return failure(error_code::invalid_path, "Path is empty");
    }
    auto syntax_result = validatePathSyntax(path, config);
    if (!syntax_result) {
        return syntax_result;
    }

    auto existence_result = validatePathExistence(path, config);
    if (!existence_result) {
        return existence_result;
    }

    std::error_code ec;
    const bool path_exists = std::filesystem::exists(path, ec);
    if (ec) {
        // A stat that genuinely FAILS (not a plain absence) leaves existence unresolved.
        // Fail closed rather than skip the permission check and fall through to success.
        return failure(error_code::filesystem_error, "Cannot determine whether path exists");
    }
    if (path_exists) {
        auto perm_result = validatePathPermissions(path, config);
        if (!perm_result) {
            return perm_result;
        }
    }

    if (!config.base_directory.empty()) {
        auto within_base = validatePathWithinBase(path, config.base_directory);
        if (!within_base) {
            return within_base;
        }
    }

    return success();
}

namespace {

bool isPathSeparator(char c) noexcept {
    return c == '/' || c == '\\';
}

// True when ".." appears as a WHOLE path segment (bounded by a separator or a
// string boundary), i.e. an actual parent-directory reference. A blanket ".."
// substring test also rejected legitimate names like "my..file" or "..bar";
// this keeps the traversal block precise while still catching "..", "../x",
// "x/..", and "a/../b" under either separator regardless of host platform.
bool hasDotDotSegment(const std::string& s) noexcept {
    std::size_t pos = 0;
    while ((pos = s.find("..", pos)) != std::string::npos) {
        const bool before_ok = pos == 0 || isPathSeparator(s[pos - 1]);
        const std::size_t after = pos + 2;
        const bool after_ok = after == s.size() || isPathSeparator(s[after]);
        if (before_ok && after_ok) {
            return true;
        }
        pos += 1;
    }
    return false;
}

}  // namespace

bool input_validator::containsTraversalSequences(const std::filesystem::path& path) noexcept {
    // An empty path contains no traversal sequence; do not assert non-empty
    // (debug-only crash on a legitimate empty query).
    const auto path_str = path.string();

    // Reject a real parent-directory segment (".." delimited by separators), not
    // any incidental ".." inside an ordinary filename.
    if (hasDotDotSegment(path_str)) {
        return true;
    }

    // Check for encoded traversal attempts
    if (path_str.find("%2e%2e") != std::string::npos ||      // URL encoded ..
        path_str.find("%252e%252e") != std::string::npos) {  // Double encoded
        return true;
    }

    // Check each path component
    return std::any_of(path.begin(), path.end(), [](const auto& component) {
        const auto comp_str = component.string();
        return comp_str == ".." || comp_str == ".";
    });
}

validation_result input_validator::validatePathWithinBase(const std::filesystem::path& path,
                                                          const std::filesystem::path& base_dir) {
    // BY CONTRACT this is a pathname (string) containment check, not a handle-based
    // one -- it cannot itself close the validate-then-open TOCTOU window. The
    // ancestor reparse-point walk in validatePathExistence blocks junction/symlink
    // redirection of the tree, and destructive callers additionally re-verify the
    // opened handle (GetFinalPathNameByHandleW) against this result. Adding a handle
    // probe here would open a path this function is not given the access intent for.
    try {
        // Canonicalize both paths
        std::error_code ec;
        auto canonical_path = std::filesystem::weakly_canonical(path, ec);
        if (ec) {
            return failure(error_code::invalid_path, "Cannot canonicalize path");
        }

        auto canonical_base = std::filesystem::weakly_canonical(base_dir, ec);
        if (ec) {
            return failure(error_code::invalid_path, "Cannot canonicalize base directory");
        }

        // Containment must be tested component-by-component. A raw string prefix
        // test accepts sibling directories that share a textual prefix -- e.g.
        // base "C:/safe" would wrongly contain "C:/safeevil/payload".
        if (!baseContainsPath(canonical_base, canonical_path)) {
            return failure(error_code::path_traversal_attempt,
                           "Path is outside allowed base directory");
        }

        return success();

    } catch (const std::exception& e) {
        sak::logWarning("Exception during path validation: {}", e.what());
        return failure(error_code::unknown_error, "Exception during path validation");
    }
}

bool input_validator::containsSuspiciousPatterns(const std::filesystem::path& path) noexcept {
    // An empty path has no suspicious pattern; do not assert non-empty (debug-only
    // crash on a legitimate empty query).
    const auto path_str = path.string();

    // Windows device names (CON, PRN, AUX, NUL, COM1-9, LPT1-9)
    static const std::regex device_pattern(kWindowsDeviceNamePattern, std::regex::icase);

    if (std::regex_search(path_str, device_pattern)) {
        return true;
    }

    // UNC paths (Windows network paths)
#ifdef _WIN32
    if (path_str.starts_with("\\\\") || path_str.starts_with("//")) {
        return true;
    }
#endif

    // Unusual characters that might indicate injection
    if (path_str.find('\0') != std::string::npos || path_str.find('\n') != std::string::npos ||
        path_str.find('\r') != std::string::npos) {
        return true;
    }

    return false;
}

// ============================================
// String Validation
// ============================================

validation_result input_validator::validateString(std::string_view str,
                                                  const string_validation_config& config) {
    validation_result result = validateStringLengthPolicy(str, config);
    if (!result) {
        return result;
    }

    result = validateStringCharacterPolicy(str, config);
    if (!result) {
        return result;
    }

    return validateStringEncodingPolicy(str, config);
}

bool input_validator::containsNullBytes(std::string_view str) noexcept {
    return str.find('\0') != std::string_view::npos;
}

bool input_validator::containsControlChars(std::string_view str) noexcept {
    return std::any_of(str.begin(), str.end(), [](unsigned char c) {
        return std::iscntrl(c) && c != '\n' && c != '\r' && c != '\t';
    });
}

bool input_validator::isValidUtf8(std::string_view str) noexcept {
    // An empty string is vacuously valid UTF-8; do not assert non-empty (that
    // crashed debug builds on a legitimate empty input). The loop below handles
    // an empty view correctly.
    std::size_t i = 0;

    while (i < str.length()) {
        const unsigned char c = static_cast<unsigned char>(str[i]);

        if (c < kAsciiLimit) {
            i++;
            continue;
        }

        const int seq_len = checkMultiByteUtf8(str, i);
        if (seq_len == 0) {
            return false;
        }
        i += seq_len;
    }

    return true;
}

std::string input_validator::sanitizeString(std::string_view str, bool allow_unicode) {
    // Sanitizing an empty string yields an empty string; do not assert non-empty
    // (a debug-only crash on legitimate input). The loop handles an empty view.
    std::string result;
    result.reserve(str.length());

    for (const unsigned char c : str) {
        if (c == '\0') {
            continue;
        }

        if ((std::iscntrl(c) != 0) && c != '\n' && c != '\r' && c != '\t') {
            continue;
        }

        if (c >= kAsciiLimit && !allow_unicode) {
            continue;
        }

        result.push_back(static_cast<char>(c));
    }

    return result;
}

// ============================================
// Buffer Validation
// ============================================

validation_result input_validator::validateBufferSize(std::size_t buffer_size,
                                                      std::size_t max_size,
                                                      std::size_t required_size) {
    // A zero maximum is a misconfiguration, not a validation that trivially passes.
    if (max_size == 0) {
        return failure(error_code::validation_failed, "Maximum buffer size must be positive");
    }

    if (required_size > 0 && buffer_size < required_size) {
        return failure(error_code::validation_failed, "Buffer is too small");
    }

    if (buffer_size > max_size) {
        return failure(error_code::validation_failed, "Buffer exceeds maximum allowed size");
    }

    return success();
}

// ============================================
// Resource Validation
// ============================================

validation_result input_validator::validate_disk_space(const std::filesystem::path& path,
                                                       std::uintmax_t required_bytes) {
    // Fail closed on an empty path or a zero byte requirement rather than run a
    // trivially-true check.
    if (path.empty() || required_bytes == 0) {
        return failure(error_code::validation_failed,
                       "Disk space check needs a path and a positive byte count");
    }

    try {
        std::error_code ec;
        auto space_info = std::filesystem::space(path, ec);

        if (ec) {
            return failure(error_code::filesystem_error, "Cannot determine available disk space");
        }

        if (space_info.available < required_bytes) {
            return failure(error_code::insufficient_disk_space,
                           "Insufficient disk space available");
        }

        return success();

    } catch (const std::exception& e) {
        sak::logWarning("Exception during disk space validation: {}", e.what());
        return failure(error_code::unknown_error, "Exception during disk space validation");
    }
}

validation_result input_validator::validate_available_memory(std::size_t required_bytes) {
    // A zero requirement is a misconfiguration.
    if (required_bytes == 0) {
        return failure(error_code::validation_failed, "Required memory must be positive");
    }

    const auto available = get_available_memory_impl();

    if (available == 0) {
        return failure(error_code::unknown_error, "Cannot determine available memory");
    }

    if (available < required_bytes) {
        return failure(error_code::insufficient_memory, "Insufficient memory available");
    }

    return success();
}

validation_result input_validator::validate_file_descriptor_limit() {
#ifdef _WIN32
    // Windows has no POSIX file descriptor limit; always succeeds
    return success();
#else
    const auto current_count = get_file_descriptor_count_impl();
    const auto limit = get_file_descriptor_limit_impl();

    if (limit == 0) {
        return success();  // Cannot determine, assume OK
    }

    // Warn if using more than 80% of limit
    if (current_count > (limit * kFileDescriptorWarnNumerator / kFileDescriptorWarnDenominator)) {
        logWarning("Approaching file descriptor limit: {}/{}", current_count, limit);
        return failure(error_code::resource_limit_reached, "Approaching file descriptor limit");
    }

    return success();
#endif
}

validation_result input_validator::validate_thread_count(std::size_t requested_threads) {
    // A zero thread request is invalid, not a trivially-satisfied validation.
    if (requested_threads == 0) {
        return failure(error_code::validation_failed, "Requested thread count must be positive");
    }

    const auto hardware_threads = std::thread::hardware_concurrency();

    if (hardware_threads == 0) {
        // Cannot determine, allow up to reasonable limit
        if (requested_threads > kUnknownHardwareThreadLimit) {
            return failure(error_code::validation_failed, "Thread count exceeds reasonable limit");
        }
        return success();
    }

    // Warn if requesting more than 2x hardware threads
    if (requested_threads > hardware_threads * kThreadWarningMultiplier) {
        logWarning("Requested threads ({}) exceeds 2x hardware threads ({})",
                   requested_threads,
                   hardware_threads);
    }

    // Reject if requesting more than 4x hardware threads
    if (requested_threads > hardware_threads * kThreadRejectMultiplier) {
        return failure(error_code::validation_failed, "Thread count exceeds 4x hardware threads");
    }

    return success();
}

// ============================================
// Helper Functions
// ============================================

validation_result input_validator::success() noexcept {
    return validation_result{.is_valid = true, .error = error_code::success, .error_message = ""};
}

validation_result input_validator::failure(error_code err, std::string_view message) {
    return validation_result{.is_valid = false,
                             .error = err,
                             .error_message = std::string(message)};
}

// ============================================
// Platform-Specific Implementations
// ============================================

std::uintmax_t input_validator::get_available_memory_impl() {
#ifdef _WIN32
    MEMORYSTATUSEX mem_info;
    mem_info.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&mem_info) != 0) {
        return mem_info.ullAvailPhys;
    }
    return 0;
#else
    long pages = sysconf(_SC_AVPHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page_size > 0) {
        return static_cast<std::uintmax_t>(pages) * static_cast<std::uintmax_t>(page_size);
    }
    return 0;
#endif
}

#ifndef _WIN32
std::size_t input_validator::get_file_descriptor_count_impl() {
    // Count open file descriptors in /proc/self/fd
    std::size_t count = 0;
    try {
        for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
            (void)entry;
            ++count;
        }
    } catch (const std::exception& e) {
        logDebug("Failed to enumerate /proc/self/fd for file descriptor count: {}", e.what());
        return 0;  // Cannot determine
    }
    return count;
}

std::size_t input_validator::get_file_descriptor_limit_impl() {
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit) == 0) {
        return limit.rlim_cur;
    }
    return 0;
}
#endif

}  // namespace sak
