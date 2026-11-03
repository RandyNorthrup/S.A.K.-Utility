// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file mbox_header_parser.h
/// @brief Pure RFC 5322 header parsing for MBOX/EML messages, split out as a seam.
///
/// The header block of a mail message is attacker-controlled bytes. The parsing was a
/// private member of MboxParser (which owns an open QFile), so it could not be exercised
/// with a raw byte buffer on its own. Lifting it to a pure, stateless free function --
/// with MboxParser::parseHeaders now delegating here -- keeps behaviour identical while
/// letting a fuzz harness feed it malformed header blocks directly (folded/continuation
/// lines, missing colons, embedded CRs, no terminating blank line).
///
/// The output contract, relied on by the fuzz invariant and by every caller: each key is
/// lower-cased and trimmed of surrounding whitespace, and an empty name is never emitted.

#pragma once

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QRegularExpression>
#include <QString>
#include <QStringDecoder>

#include <optional>

namespace sak::mbox {

/// Parse the RFC 5322 header block at the front of @p raw_message into a name->value map.
/// Header names are lower-cased and trimmed; folded continuation lines (leading space or
/// tab) are joined onto the preceding value; parsing stops at the first blank line.
[[nodiscard]] inline QMap<QString, QString> parseRfc5322Headers(const QByteArray& raw_message) {
    QMap<QString, QString> headers;

    // Find the header/body boundary.
    int header_end = static_cast<int>(raw_message.indexOf("\r\n\r\n"));
    if (header_end < 0) {
        header_end = static_cast<int>(raw_message.indexOf("\n\n"));
    }

    const QByteArray header_block = (header_end >= 0) ? raw_message.left(header_end) : raw_message;

    // Split into lines and handle continuation (folded headers).
    const QList<QByteArray> lines = header_block.split('\n');

    QString current_name;
    QString current_value;

    auto flushHeader = [&]() {
        // Normalize BEFORE the guard, not after. Guarding on the raw name let a name that is
        // non-empty but consists only of whitespace through, and it was then inserted under a key
        // that trimmed to empty -- breaking the output contract above ("an empty name is never
        // emitted") for any caller iterating the map, which then sees a header it cannot key on.
        // Such a name is reachable: the continuation check below treats only ' ' and '\t' as an
        // indent, while QString::trimmed() also strips \v, \f and \r, so a line like "\v: x" is
        // parsed as a new header whose name survives the raw guard and vanishes at insert.
        const QString key = current_name.toLower().trimmed();
        if (!key.isEmpty()) {
            headers.insert(key, current_value.trimmed());
        }
        current_name.clear();
        current_value.clear();
    };

    for (const auto& raw_line : lines) {
        QByteArray line = raw_line;
        if (line.endsWith('\r')) {
            line.chop(1);
        }

        if (line.isEmpty()) {
            break;
        }

        // Continuation line (starts with whitespace).
        if (line.startsWith(' ') || line.startsWith('\t')) {
            current_value += QLatin1Char(' ') + QString::fromUtf8(line).trimmed();
            continue;
        }

        // New header.
        flushHeader();
        const int colon = static_cast<int>(line.indexOf(':'));
        if (colon > 0) {
            current_name = QString::fromUtf8(line.left(colon));
            current_value = QString::fromUtf8(line.mid(colon + 1)).trimmed();
        }
    }
    flushHeader();

    return headers;
}

namespace detail {

constexpr int kHexBase = 16;
constexpr qsizetype kQHexDigits = 2;

/// RFC 2047 Q encoding: '_' is a space and =XX is a byte. Nullopt on a stray '='.
[[nodiscard]] inline std::optional<QByteArray> decodeQEncoding(const QByteArray& text) {
    QByteArray bytes;
    bytes.reserve(text.size());
    for (qsizetype at = 0; at < text.size(); ++at) {
        const char ch = text.at(at);
        if (ch == '_') {
            bytes.append(' ');
        } else if (ch != '=') {
            bytes.append(ch);
        } else {
            bool ok = false;
            const QByteArray digits = text.mid(at + 1, kQHexDigits);
            const int value = digits.toInt(&ok, kHexBase);
            if (!ok || digits.size() != kQHexDigits) {
                return std::nullopt;
            }
            bytes.append(static_cast<char>(value));
            at += kQHexDigits;
        }
    }
    return bytes;
}

/// RFC 2047 B encoding. Real senders often leave off the padding, which Qt reads without complaint.
[[nodiscard]] inline std::optional<QByteArray> decodeBEncoding(const QByteArray& text) {
    auto decoded = QByteArray::fromBase64Encoding(text, QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded) {
        return std::nullopt;
    }
    return std::move(decoded.decoded);
}

/// The text @p bytes spell in @p charset. Nullopt when the charset is unknown or they do not fit
/// it.
[[nodiscard]] inline std::optional<QString> decodeWordBytes(const QByteArray& bytes,
                                                            const QString& charset) {
    // Stateless: a word ending partway through a character is an error, not something to wait on.
    QStringDecoder decoder(charset.toLatin1().constData(), QStringConverter::Flag::Stateless);
    if (!decoder.isValid()) {
        return std::nullopt;
    }
    QString text = decoder.decode(bytes);
    if (decoder.hasError()) {
        return std::nullopt;
    }
    return text;
}

/// A run of adjacent words in one charset, decoded together: senders split a character across two.
struct EncodedWordRun {
    bool active = false;
    QString charset;
    QByteArray bytes;
    qsizetype raw_start = 0;
    qsizetype raw_end = 0;
};

inline void flushEncodedWordRun(EncodedWordRun& run, const QString& value, QString& out) {
    if (!run.active) {
        return;
    }
    const std::optional<QString> text = decodeWordBytes(run.bytes, run.charset);
    out += text.has_value() ? *text : value.mid(run.raw_start, run.raw_end - run.raw_start);
    run = {};
}

/// One encoded word: its charset, without the language RFC 2231 lets it carry after '*', and its
/// bytes. Nullopt bytes when the encoding does not decode.
struct EncodedWord {
    QString charset;
    std::optional<QByteArray> bytes;
};

[[nodiscard]] inline EncodedWord parseEncodedWord(const QRegularExpressionMatch& match) {
    constexpr int kCharsetGroup = 1;
    constexpr int kEncodingGroup = 2;
    constexpr int kTextGroup = 3;
    const QByteArray text = match.captured(kTextGroup).toLatin1();
    const bool base64 =
        match.captured(kEncodingGroup).compare(QLatin1String("b"), Qt::CaseInsensitive) == 0;
    return {.charset = match.captured(kCharsetGroup).section(QLatin1Char('*'), 0, 0),
            .bytes = base64 ? decodeBEncoding(text) : decodeQEncoding(text)};
}

/// A header value stays on one line, whatever an encoded word smuggled into it.
inline void blankControlCharacters(QString& text) {
    for (QChar& ch : text) {
        if (ch.category() == QChar::Other_Control && ch != QLatin1Char('\t')) {
            ch = QLatin1Char(' ');
        }
    }
}

}  // namespace detail

/// The text of a header value holding RFC 2047 encoded words, =?charset?B|Q?text?=. Adjacent words
/// join with the white space between them dropped, which covers a folded header. A word with a
/// charset this system does not know, or an encoding that does not decode, stays exactly as
/// written: the raw text is better than nothing, and reading a mailbox must not fail over it.
[[nodiscard]] inline QString decodeEncodedWords(const QString& value) {
    if (!value.contains(QStringLiteral("=?"))) {
        return value;
    }
    static const QRegularExpression word(
        QStringLiteral(R"re(=\?([^?\s]+)\?([BbQq])\?(\S*?)\?=)re"));
    QString out;
    qsizetype copied = 0;
    detail::EncodedWordRun run;
    auto matches = word.globalMatch(value);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        const QString between = value.mid(copied, match.capturedStart() - copied);
        const bool adjacent = run.active && between.trimmed().isEmpty();
        const detail::EncodedWord parsed = detail::parseEncodedWord(match);
        copied = match.capturedEnd();
        if (adjacent && parsed.bytes.has_value() &&
            parsed.charset.compare(run.charset, Qt::CaseInsensitive) == 0) {
            run.bytes += *parsed.bytes;
            run.raw_end = copied;
            continue;
        }
        detail::flushEncodedWordRun(run, value, out);
        if (!adjacent || !parsed.bytes.has_value()) {
            out += between;
        }
        if (parsed.bytes.has_value()) {
            run = {.active = true,
                   .charset = parsed.charset,
                   .bytes = *parsed.bytes,
                   .raw_start = match.capturedStart(),
                   .raw_end = copied};
        } else {
            out += match.captured(0);
        }
    }
    detail::flushEncodedWordRun(run, value, out);
    out += value.mid(copied);
    detail::blankControlCharacters(out);
    return out;
}

}  // namespace sak::mbox
