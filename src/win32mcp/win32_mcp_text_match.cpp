// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_text_match.h"

#include <QHash>
#include <QLatin1Char>
#include <QStringList>

#include <algorithm>

namespace sak::win32mcp {

namespace {

// Lowercase a word and strip leading/trailing non-alphanumerics so OCR punctuation ("Continue:",
// "cookies,") does not defeat a whole-word comparison.
QString normWord(const QString& raw) {
    const QString s = raw.toLower();
    int a = 0;
    int b = static_cast<int>(s.size());
    while (a < b && !s[a].isLetterOrNumber()) {
        ++a;
    }
    while (b > a && !s[b - 1].isLetterOrNumber()) {
        --b;
    }
    return s.mid(a, b - a);
}

// Match strength of the token run starting at hits[i]: 2 = every token is a WHOLE-WORD match,
// 1 = at least one token only matched as a substring (allowed only when allow_sub), 0 = no match.
// On a non-zero return m holds the union box, joined text, and source line of the run.
int runMatch(
    const QVector<WordHit>& hits, int i, const QStringList& tokens, bool allow_sub, TextMatch& m) {
    const int line = hits[i].line;
    int x0 = hits[i].x;
    int y0 = hits[i].y;
    int x1 = hits[i].x + hits[i].w;
    int y1 = hits[i].y + hits[i].h;
    int strength = 2;
    QStringList joined;
    for (int t = 0; t < tokens.size(); ++t) {
        const WordHit& h = hits[i + t];
        const QString w = normWord(h.text);
        if (h.line != line || w.isEmpty()) {
            return 0;
        }
        if (w != tokens[t]) {
            if (!(allow_sub && w.contains(tokens[t]))) {
                return 0;
            }
            strength = 1;
        }
        x0 = std::min(x0, h.x);
        y0 = std::min(y0, h.y);
        x1 = std::max(x1, h.x + h.w);
        y1 = std::max(y1, h.y + h.h);
        joined << h.text;
    }
    m = TextMatch{joined.join(QLatin1Char(' ')), x0, y0, x1 - x0, y1 - y0, line, 0};
    return strength;
}

}  // namespace

QVector<TextMatch> locateText(const QVector<WordHit>& hits, const QString& query, bool allow_sub) {
    QVector<TextMatch> out;
    QStringList tokens;
    const QStringList parts = query.toLower().simplified().split(QLatin1Char(' '),
                                                                 Qt::SkipEmptyParts);
    for (const QString& p : parts) {
        const QString t = normWord(p);
        if (!t.isEmpty()) {
            tokens.append(t);
        }
    }
    if (tokens.isEmpty()) {
        return out;
    }
    QHash<int, int> line_words;
    for (const WordHit& h : hits) {
        ++line_words[h.line];
    }
    const int n = static_cast<int>(hits.size());
    const int tk = static_cast<int>(tokens.size());
    for (int i = 0; i + tk <= n; ++i) {
        TextMatch m;
        const int strength = runMatch(hits, i, tokens, allow_sub, m);
        if (strength == 0) {
            continue;
        }
        const int extra = line_words.value(m.line, tk) - tk;  // other words sharing the line
        m.score = strength * 100'000 - extra;
        out.append(m);
    }
    std::stable_sort(out.begin(), out.end(), [](const TextMatch& a, const TextMatch& b) {
        return a.score > b.score;
    });
    return out;
}

}  // namespace sak::win32mcp
