// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef SAK_WIN32MCP_WIN32_MCP_TEXT_MATCH_H
#define SAK_WIN32MCP_WIN32_MCP_TEXT_MATCH_H

#include <QString>
#include <QVector>

namespace sak::win32mcp {

// One recognized OCR word with an absolute screen box and the index of the source line it came
// from (words sharing a line have the same `line`; used to require a multi-word query to match a
// consecutive same-line run and to rank a standalone label above a word buried in a sentence).
struct WordHit {
    QString text;
    int x{0};
    int y{0};
    int w{0};
    int h{0};
    int line{0};
};

// A located run of OCR text: the joined caption, its combined screen box, the source line, and a
// rank score (higher is better).
struct TextMatch {
    QString text;
    int x{0};
    int y{0};
    int w{0};
    int h{0};
    int line{0};
    int score{0};
};

// Locate `query` in the OCR word hits, best match first.
//
// Tokens must match WHOLE OCR words (case- and punctuation-insensitive), so "OK" never matches
// "cookies" and "Scan" never matches "Scanning". A multi-word query must match a consecutive run
// of words on the SAME line. Results are ranked so a whole-word hit always beats a substring hit,
// and among equal strength a standalone label (its own short line, e.g. a button) beats the same
// word buried in a longer sentence.
//
// Substring matching is opt-in via `allow_sub` (the caller's `contains` flag) and can never
// outrank a whole-word hit. Returns an empty list when the query is blank or nothing matches.
QVector<TextMatch> locateText(const QVector<WordHit>& hits, const QString& query, bool allow_sub);

}  // namespace sak::win32mcp

#endif  // SAK_WIN32MCP_WIN32_MCP_TEXT_MATCH_H
