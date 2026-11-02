// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file pst_rtf_body.cpp
/// @brief MS-OXRTFCP decompression and RTF text extraction.

#include "sak/pst_rtf_body.h"

#include <QByteArrayView>
#include <QHash>
#include <QStringDecoder>
#include <QtEndian>

#include <array>
#include <cctype>
#include <string_view>
#include <vector>

namespace sak::pst {

namespace {

// MS-OXRTFCP 2.1.3.1.1: COMPSIZE, RAWSIZE, COMPTYPE, CRC, then the stream.
constexpr int kHeaderBytes = 16;
constexpr int kHeaderRawSizeOffset = 4;
constexpr int kHeaderTypeOffset = 8;
constexpr int kHeaderCrcOffset = 12;
constexpr int kCompSizeFieldBytes = 4;
constexpr uint32_t kTypeCompressed = 0x75'46'5A'4C;    // "LZFu"
constexpr uint32_t kTypeUncompressed = 0x41'4C'45'4D;  // "MELA"
// A body beyond this is not a mail body. It bounds the output of a hostile RAWSIZE.
constexpr uint32_t kMaxRawSize = 256U * 1024 * 1024;

// The 4096-byte dictionary starts with this prebuilt text (MS-OXRTFCP 2.1.2.1).
constexpr std::string_view kPrebuiltDictionary =
    "{\\rtf1\\ansi\\mac\\deff0\\deftab720{\\fonttbl;}{\\f0\\fnil \\froman \\fswiss \\fmodern "
    "\\fscript \\fdecor MS Sans SerifSymbolArialTimes New RomanCourier{\\colortbl\\red0\\green0"
    "\\blue0\r\n\\par \\pard\\plain\\f0\\fs20\\b\\i\\u\\tab\\tx";
constexpr int kDictionaryBytes = 4096;
constexpr int kDictionaryMask = kDictionaryBytes - 1;
constexpr int kControlBits = 8;
constexpr int kReferenceOffsetShift = 4;
constexpr int kReferenceLengthMask = 0x0F;
constexpr int kReferenceMinLength = 2;
constexpr int kByteBits = 8;
constexpr int kReferenceBytes = 2;

uint32_t rtfCrc(const QByteArray& data, int offset) {
    static const std::array<uint32_t, 256> kTable = [] {
        std::array<uint32_t, 256> table{};
        for (uint32_t i = 0; i < table.size(); ++i) {
            uint32_t c = i;
            for (int bit = 0; bit < kByteBits; ++bit) {
                c = (c & 1U) != 0 ? (0xED'B8'83'20U ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        return table;
    }();
    uint32_t crc = 0;
    for (qsizetype i = offset; i < data.size(); ++i) {
        crc = kTable[(crc ^ static_cast<uint8_t>(data.at(i))) & 0xFFU] ^ (crc >> kByteBits);
    }
    return crc;
}

/// Run the LZ77 stream. Returns false on a stream that ends early or runs past @p raw_size.
bool inflateStream(const QByteArray& input, uint32_t raw_size, QByteArray& out) {
    std::array<char, kDictionaryBytes> dictionary{};
    std::copy(kPrebuiltDictionary.begin(), kPrebuiltDictionary.end(), dictionary.begin());
    int write_pos = static_cast<int>(kPrebuiltDictionary.size());

    qsizetype pos = kHeaderBytes;
    while (pos < input.size()) {
        const auto control = static_cast<uint8_t>(input.at(pos++));
        for (int bit = 0; bit < kControlBits; ++bit) {
            if ((control & (1U << bit)) == 0) {
                if (pos >= input.size()) {
                    return false;
                }
                const char literal = input.at(pos++);
                dictionary[static_cast<size_t>(write_pos)] = literal;
                write_pos = (write_pos + 1) & kDictionaryMask;
                out.append(literal);
                continue;
            }
            if (pos + kReferenceBytes > input.size()) {
                return false;
            }
            const uint16_t reference = qFromBigEndian<uint16_t>(input.constData() + pos);
            pos += kReferenceBytes;
            const int read_start = reference >> kReferenceOffsetShift;
            const int length = (reference & kReferenceLengthMask) + kReferenceMinLength;
            // A reference to the write position ends the stream.
            if (read_start == write_pos) {
                return out.size() == static_cast<qsizetype>(raw_size);
            }
            for (int i = 0; i < length; ++i) {
                const char value =
                    dictionary[static_cast<size_t>((read_start + i) & kDictionaryMask)];
                dictionary[static_cast<size_t>(write_pos)] = value;
                write_pos = (write_pos + 1) & kDictionaryMask;
                out.append(value);
            }
            if (out.size() > static_cast<qsizetype>(raw_size)) {
                return false;
            }
        }
    }
    return false;
}

// ------------------------------------------------------------------------------------------------
// RTF text extraction
// ------------------------------------------------------------------------------------------------

constexpr int kDefaultAnsiCodePage = 1252;
constexpr int kHexPairChars = 2;
constexpr int kHexBase = 16;
constexpr int kUnicodeWrapModulus = 65'536;
constexpr int kMaxDestinationDepth = 512;

/// Destinations holding no readable text.
bool isHiddenDestination(const QByteArray& word) {
    static const std::array<std::string_view, 12> kHidden = {
        "fonttbl",
        "colortbl",
        "stylesheet",
        "info",
        "pict",
        "object",
        "themedata",
        "colorschememapping",
        "latentstyles",
        "datastore",
        "listtable",
        "listoverridetable",
    };
    for (const auto hidden : kHidden) {
        if (word == QByteArrayView(hidden.data(), static_cast<qsizetype>(hidden.size()))) {
            return true;
        }
    }
    return false;
}

/// Group state RTF scopes with braces.
struct RtfGroup {
    bool hidden = false;
    bool html_suppressed = false;
    int unicode_skip = 1;
};

class RtfTextExtractor {
public:
    explicit RtfTextExtractor(const QByteArray& rtf) : m_rtf(rtf) { m_groups.push_back({}); }

    RtfBodyText run() {
        while (m_pos < m_rtf.size()) {
            const char c = m_rtf.at(m_pos);
            if (c == '{') {
                ++m_pos;
                pushGroup();
            } else if (c == '}') {
                ++m_pos;
                popGroup();
            } else if (c == '\\') {
                ++m_pos;
                readControl();
            } else if (c == '\r' || c == '\n') {
                ++m_pos;
            } else {
                ++m_pos;
                appendByte(c);
            }
        }
        flushBytes();
        return RtfBodyText{.text = m_text, .is_html = m_is_html};
    }

private:
    [[nodiscard]] RtfGroup& group() { return m_groups.back(); }
    [[nodiscard]] bool suppressed() const {
        return m_groups.back().hidden || m_groups.back().html_suppressed;
    }

    void pushGroup() {
        if (static_cast<int>(m_groups.size()) >= kMaxDestinationDepth) {
            // Deeper than any real body; treat what follows as hidden rather than recurse on.
            m_groups.push_back(RtfGroup{.hidden = true});
            return;
        }
        m_groups.push_back(m_groups.back());
    }

    void popGroup() {
        flushBytes();
        if (m_groups.size() > 1) {
            m_groups.pop_back();
        }
    }

    void appendByte(char c) {
        if (m_skip_chars > 0) {
            --m_skip_chars;
            return;
        }
        if (!suppressed()) {
            m_pending.append(c);
        }
    }

    void flushBytes() {
        if (m_pending.isEmpty()) {
            return;
        }
        const QByteArray name = QByteArray("CP") + QByteArray::number(m_code_page);
        QStringDecoder decoder(name.constData());
        m_text += decoder.isValid() ? decoder.decode(m_pending) : QString::fromLatin1(m_pending);
        m_pending.clear();
    }

    void appendText(const QString& text) {
        if (!suppressed()) {
            flushBytes();
            m_text += text;
        }
    }

    void readControl() {
        if (m_pos >= m_rtf.size()) {
            return;
        }
        const char c = m_rtf.at(m_pos);
        if (std::isalpha(static_cast<unsigned char>(c)) == 0) {
            ++m_pos;
            readControlSymbol(c);
            return;
        }
        QByteArray word;
        while (m_pos < m_rtf.size() && std::isalpha(static_cast<unsigned char>(m_rtf.at(m_pos)))) {
            word.append(m_rtf.at(m_pos++));
        }
        const std::optional<int> param = readParam();
        if (m_pos < m_rtf.size() && m_rtf.at(m_pos) == ' ') {
            ++m_pos;
        }
        applyControlWord(word, param);
    }

    [[nodiscard]] std::optional<int> readParam() {
        const qsizetype start = m_pos;
        if (m_pos < m_rtf.size() && m_rtf.at(m_pos) == '-') {
            ++m_pos;
        }
        while (m_pos < m_rtf.size() && std::isdigit(static_cast<unsigned char>(m_rtf.at(m_pos)))) {
            ++m_pos;
        }
        bool ok = false;
        const int value = m_rtf.mid(start, m_pos - start).toInt(&ok);
        return ok ? std::optional<int>(value) : std::nullopt;
    }

    void readHexEscape() {
        if (m_pos + kHexPairChars > m_rtf.size()) {
            m_pos = m_rtf.size();
            return;
        }
        bool ok = false;
        const int value = m_rtf.mid(m_pos, kHexPairChars).toInt(&ok, kHexBase);
        m_pos += kHexPairChars;
        if (ok) {
            appendByte(static_cast<char>(value));
        }
    }

    void readControlSymbol(char c) {
        if (c == '*') {
            // "{\*\unknown ...}": an ignorable destination. Encapsulated HTML hides its tags in
            // \*\htmltag, which are the very content wanted here, so that one stays visible.
            m_ignorable_next = true;
            return;
        }
        m_ignorable_next = false;
        if (c == '\'') {
            readHexEscape();
        } else if (c == '{' || c == '}' || c == '\\') {
            appendByte(c);
        } else if (c == '~') {
            appendText(QStringLiteral(" "));
        } else if (c == '-' || c == '_') {
            appendText(QStringLiteral("-"));
        }
    }

    void applyControlWord(const QByteArray& word, std::optional<int> param) {
        const bool ignorable = m_ignorable_next;
        m_ignorable_next = false;
        if (word == "htmltag" || applyStateWord(word, param)) {
            return;
        }
        if (ignorable || isHiddenDestination(word)) {
            flushBytes();
            group().hidden = true;
            return;
        }
        applyTextWord(word);
    }

    /// Control words that change how what follows is read. Returns false for any other word.
    bool applyStateWord(const QByteArray& word, std::optional<int> param) {
        if (word == "fromhtml") {
            m_is_html = param.value_or(0) != 0;
        } else if (word == "htmlrtf") {
            // \htmlrtf marks RTF-only text an encapsulated HTML reader must skip; \htmlrtf0 ends
            // it.
            group().html_suppressed = param.value_or(1) != 0;
        } else if (word == "ansicpg" && param.has_value()) {
            flushBytes();
            m_code_page = *param;
        } else if (word == "uc" && param.has_value()) {
            group().unicode_skip = *param;
        } else if (word == "u" && param.has_value()) {
            appendUnicode(*param);
        } else {
            return false;
        }
        return true;
    }

    void appendUnicode(int code) {
        const int unit = code < 0 ? code + kUnicodeWrapModulus : code;
        appendText(QString(QChar(static_cast<char16_t>(unit))));
        m_skip_chars = group().unicode_skip;
    }

    void applyTextWord(const QByteArray& word) {
        static const QHash<QByteArray, QString> kTextWords = {
            {"par", QStringLiteral("\n")},
            {"line", QStringLiteral("\n")},
            {"row", QStringLiteral("\n")},
            {"sect", QStringLiteral("\n")},
            {"page", QStringLiteral("\n")},
            {"tab", QStringLiteral("\t")},
            {"cell", QStringLiteral("\t")},
            {"emdash", QString(QChar(0x2014))},
            {"endash", QString(QChar(0x2013))},
            {"bullet", QString(QChar(0x2022))},
            {"lquote", QString(QChar(0x2018))},
            {"rquote", QString(QChar(0x2019))},
            {"ldblquote", QString(QChar(0x201C))},
            {"rdblquote", QString(QChar(0x201D))},
        };
        const auto it = kTextWords.constFind(word);
        if (it != kTextWords.constEnd()) {
            appendText(*it);
        }
    }

    const QByteArray& m_rtf;
    qsizetype m_pos = 0;
    std::vector<RtfGroup> m_groups;
    QString m_text;
    QByteArray m_pending;
    int m_code_page = kDefaultAnsiCodePage;
    int m_skip_chars = 0;
    bool m_is_html = false;
    bool m_ignorable_next = false;
};

}  // namespace

std::optional<QByteArray> decompressRtf(const QByteArray& compressed) {
    if (compressed.size() < kHeaderBytes) {
        return std::nullopt;
    }
    const char* data = compressed.constData();
    const uint32_t comp_size = qFromLittleEndian<uint32_t>(data);
    const uint32_t raw_size = qFromLittleEndian<uint32_t>(data + kHeaderRawSizeOffset);
    const uint32_t type = qFromLittleEndian<uint32_t>(data + kHeaderTypeOffset);
    const uint32_t crc = qFromLittleEndian<uint32_t>(data + kHeaderCrcOffset);
    // COMPSIZE counts everything after its own field.
    const qint64 declared_total = static_cast<qint64>(comp_size) + kCompSizeFieldBytes;
    if (declared_total > compressed.size() || raw_size > kMaxRawSize) {
        return std::nullopt;
    }
    if (type == kTypeUncompressed) {
        QByteArray raw = compressed.mid(kHeaderBytes);
        if (raw.size() < static_cast<qsizetype>(raw_size)) {
            return std::nullopt;
        }
        raw.truncate(raw_size);
        return raw;
    }
    if (type != kTypeCompressed) {
        return std::nullopt;
    }
    const QByteArray stream = compressed.left(declared_total);
    if (rtfCrc(stream, kHeaderBytes) != crc) {
        return std::nullopt;
    }
    QByteArray out;
    out.reserve(raw_size);
    if (!inflateStream(stream, raw_size, out)) {
        return std::nullopt;
    }
    return out;
}

RtfBodyText rtfToText(const QByteArray& rtf) {
    RtfTextExtractor extractor(rtf);
    RtfBodyText body = extractor.run();
    body.text = body.text.trimmed();
    return body;
}

}  // namespace sak::pst
