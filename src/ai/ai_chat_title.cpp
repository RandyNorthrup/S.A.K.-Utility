// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_chat_title.h"

#include "sak/ai/ai_secret_redaction.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <array>
#include <initializer_list>

namespace sak::ai {

namespace {

constexpr int kMaxTitleWords = 6;
constexpr qsizetype kUppercaseAcronymMaxChars = 6;
constexpr qsizetype kMeaningfulWordMinChars = 2;
// A title only ever uses the opening handful of words, so the scan window is bounded here.
// Without it an arbitrarily large first prompt would drive every regex pass, split, and
// per-word loop over the whole input on the caller's (often GUI) thread.
constexpr qsizetype kMaxPromptScanChars = 4096;

QString boundedTitle(QString title) {
    title = title.simplified();
    if (title.size() <= kGeneratedChatTitleMaxChars) {
        return title;
    }

    QStringList words = title.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    while (words.size() > 1 && words.join(QLatin1Char(' ')).size() > kGeneratedChatTitleMaxChars) {
        words.removeLast();
    }
    title = words.join(QLatin1Char(' ')).left(kGeneratedChatTitleMaxChars).trimmed();
    return title.isEmpty() ? QStringLiteral("AI Chat") : title;
}

QString redactedPromptText(QString text) {
    // Bound the work before any processing; truncating the scan window changes no
    // normal-length title (see kMaxPromptScanChars).
    if (text.size() > kMaxPromptScanChars) {
        text.truncate(kMaxPromptScanChars);
    }
    text.replace(QRegularExpression(QStringLiteral(R"(```[\s\S]*?```)")), QStringLiteral(" "));
    text.replace(QRegularExpression(QStringLiteral(R"(`[^`]*`)")), QStringLiteral(" "));
    // An UNCLOSED fence or backtick is not removed by the paired patterns above -- they need a
    // closing delimiter -- so a prompt that opens a code block and never closes it (an
    // interrupted paste, or the scan window truncating mid-block) had its whole code body feed
    // the title. Drop from an unmatched opener to the end for both spellings.
    text.replace(QRegularExpression(QStringLiteral(R"(```[\s\S]*$)")), QStringLiteral(" "));
    text.replace(QRegularExpression(QStringLiteral(R"(`[^`]*$)")), QStringLiteral(" "));
    // ANY scheme, not just http(s). The old pattern let ftp://user:pass@host/path through
    // verbatim -- hostname, path and any embedded credentials -- into a title that is PERSISTED
    // with the conversation. A scheme is a letter followed by letters/digits/+/-/. per RFC 3986;
    // requiring the "://" keeps a drive letter ("C:/temp") out of this rule, which the path
    // pattern below handles instead.
    text.replace(QRegularExpression(QStringLiteral(R"([A-Za-z][A-Za-z0-9+.\-]*://\S+|www\.\S+)"),
                                    QRegularExpression::CaseInsensitiveOption),
                 QStringLiteral(" website "));
    // A drive-rooted path may contain SPACES ("C:\Users\Username With Space\secret.txt"). The old
    // pattern stopped at the first whitespace, so it redacted only up to that space and left the
    // rest of the path -- INCLUDING THE FILENAME -- in a title that is then PERSISTED: a leak in
    // exactly the artifact that outlives the conversation.
    //
    // A space is consumed only when what follows still looks like path interior, i.e. a further
    // separator appears before the next whitespace. So "C:\temp and then reboot" still stops at
    // "C:\temp" rather than swallowing the sentence: over-redaction would cost title quality,
    // under-redaction costs a leak, and this keeps both bounded.
    text.replace(QRegularExpression(
                     QStringLiteral(R"(\b[A-Za-z]:[\\/](?:[^\s]|\s(?=[^\s]*[\\/]))*)")),
                 QStringLiteral(" file "));
    text.replace(QRegularExpression(QStringLiteral(R"(\\\\[^\s]+)")), QStringLiteral(" file "));
    // Route the whole text through the ONE hardened redactor rather than keeping a second, weaker
    // secret catalogue here. This file recognised only sk-* keys, so a bearer token, a JWT, a
    // GitHub/AWS/Slack/Stripe key or a plain "password=..." went into the generated title -- and
    // a title is PERSISTED with the conversation, so it outlives the transcript it came from.
    // CredentialStore::redactSecrets covers all of those and is directly tested; a private copy
    // of a secret catalogue is exactly the drift this campaign keeps finding.
    //
    // Its marker is mapped to this file's vocabulary BEFORE the punctuation strip below, which
    // would otherwise remove the brackets and leave the bare word "redacted" in the title.
    text = sak::ai::redactSecrets(text);
    text.replace(QStringLiteral("[redacted]"), QStringLiteral(" secret "));
    text.replace(QRegularExpression(QStringLiteral(R"(\[redacted-[a-z0-9-]+\])"),
                                    QRegularExpression::CaseInsensitiveOption),
                 QStringLiteral(" secret "));
    text.replace(QRegularExpression(QStringLiteral(R"(\bsk(?:-proj)?-[A-Za-z0-9_\-]{12,}\b)"),
                                    QRegularExpression::CaseInsensitiveOption),
                 QStringLiteral(" secret "));
    // redactSecrets renders an OpenAI key as "sk-...[redacted]", whose "sk-..." prefix survives
    // the mapping above; collapse that remnant so it does not read as a truncated key.
    text.replace(QStringLiteral("sk-... secret "), QStringLiteral(" secret "));
    text.replace(QRegularExpression(
                     QStringLiteral(R"(\b[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}\b)")),
                 QStringLiteral(" account "));
    text.replace(QRegularExpression(QStringLiteral(R"([#>*_\[\]{}"'=;:,.!?/\\|]+)")),
                 QStringLiteral(" "));
    return text.simplified();
}

QSet<QString> stopWords() {
    return {
        QStringLiteral("a"),      QStringLiteral("about"), QStringLiteral("after"),
        QStringLiteral("an"),     QStringLiteral("and"),   QStringLiteral("are"),
        QStringLiteral("as"),     QStringLiteral("at"),    QStringLiteral("be"),
        QStringLiteral("can"),    QStringLiteral("could"), QStringLiteral("do"),
        QStringLiteral("does"),   QStringLiteral("for"),   QStringLiteral("from"),
        QStringLiteral("help"),   QStringLiteral("i"),     QStringLiteral("in"),
        QStringLiteral("is"),     QStringLiteral("it"),    QStringLiteral("me"),
        QStringLiteral("my"),     QStringLiteral("need"),  QStringLiteral("of"),
        QStringLiteral("on"),     QStringLiteral("or"),    QStringLiteral("our"),
        QStringLiteral("please"), QStringLiteral("the"),   QStringLiteral("this"),
        QStringLiteral("to"),     QStringLiteral("using"), QStringLiteral("want"),
        QStringLiteral("we"),     QStringLiteral("with"),  QStringLiteral("would"),
        QStringLiteral("you"),    QStringLiteral("your"),
    };
}

// True when @p lower_text contains one of @p terms AS A WHOLE WORD (or whole phrase).
//
// Plain substring matching mis-titled prompts, and did it in the direction that hurts: "virus"
// is a substring of "ANTIVIRUS", so "my antivirus is blocking the printer" came back "Malware
// Cleanup" -- the opposite of what the user said, and it never reached the printer rule because
// the malware rule is earlier in the list. Same shape for "adware" inside longer words. The
// boundary is asserted on both ends so multi-word terms ("blue screen", "ai panel") still match,
// and a term ending in a non-word character would still anchor correctly.
bool containsAnyTerm(const QString& lower_text, std::initializer_list<const char*> terms) {
    return std::ranges::any_of(terms, [&](const char* term) {
        const QString pattern = QStringLiteral("(?<![\\p{L}\\p{N}])") +
                                QRegularExpression::escape(QString::fromLatin1(term)) +
                                QStringLiteral("(?![\\p{L}\\p{N}])");
        const QRegularExpression rx(pattern, QRegularExpression::UseUnicodePropertiesOption);
        return rx.match(lower_text).hasMatch();
    });
}

QString titleCaseWord(const QString& word) {
    if (word.isEmpty()) {
        return {};
    }
    const bool keep_upper = std::ranges::any_of(word, [](QChar ch) { return ch.isDigit(); }) ||
                            (word.size() <= kUppercaseAcronymMaxChars && word == word.toUpper());
    if (keep_upper) {
        return word.toUpper();
    }
    QString out = word.toLower();
    out[0] = out.at(0).toUpper();
    return out;
}

QStringList meaningfulWords(const QString& text) {
    const QSet<QString> stops = stopWords();
    QStringList out;
    const QStringList raw = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (QString word : raw) {
        // Keep LETTERS AND DIGITS IN ANY SCRIPT, not just A-Za-z0-9. The ASCII-only class
        // erased every character of a Cyrillic, Greek, Hebrew or CJK prompt, so an otherwise
        // perfectly descriptive request produced no meaningful words at all and fell back to
        // the generic "AI Chat" -- the title was worst exactly for the users least able to
        // work around it.
        word.remove(QRegularExpression(QStringLiteral(R"([^\p{L}\p{N}+\-.])"),
                                       QRegularExpression::UseUnicodePropertiesOption));
        word = word.trimmed();
        if (word.size() < kMeaningfulWordMinChars) {
            continue;
        }
        // A token must carry at least one letter or digit to be a WORD. The class above keeps
        // '+', '-' and '.' because they belong inside real tokens (C++, wi-fi, 10.0.1), but on
        // their own they spell nothing: "---" and "++" passed the length check and counted as
        // meaningful, which is enough to defeat the low-signal fallback and produce a title made
        // of punctuation.
        static const QRegularExpression kHasAlnum(QStringLiteral(R"([\p{L}\p{N}])"),
                                                  QRegularExpression::UseUnicodePropertiesOption);
        if (!kHasAlnum.match(word).hasMatch()) {
            continue;
        }
        const QString lower = word.toLower();
        if (stops.contains(lower) || lower == QLatin1String("secret") ||
            lower == QLatin1String("file") || lower == QLatin1String("account")) {
            continue;
        }
        out.append(word);
    }
    return out;
}

// The product name at the head of an offline-installer request, title-cased, or empty when the
// prompt names no product.
//
// KEEP THE WHOLE NAME, not just its first token. Taking the first non-skipped word turned
// "download Google Chrome offline installer" into "Google Offline Installer", which names the
// wrong product -- Google ships several installers and Chrome is the one that was asked for.
//
// A product's continuation tokens are capitalised in the prompt ("Google Chrome", "Adobe
// Reader"), so continuation stops at the first lowercase word. That keeps "find an offline
// installer for Firefox but do not install it" at "Firefox" rather than swallowing "but", and it
// stops at a skip word regardless of case. THE LIMIT IS HONEST: an all-lowercase product name
// ("vlc media player") still yields only its first token, because nothing in the text
// distinguishes the rest of the name from ordinary prose without a product dictionary.
QString productNameFromWords(const QStringList& words, const QSet<QString>& skip) {
    constexpr qsizetype kMaxProductNameWords = 3;
    for (qsizetype i = 0; i < words.size(); ++i) {
        if (skip.contains(words.at(i).toLower())) {
            continue;
        }
        QStringList product{titleCaseWord(words.at(i))};
        for (qsizetype j = i + 1; j < words.size() && product.size() < kMaxProductNameWords; ++j) {
            const QString& next = words.at(j);
            if (skip.contains(next.toLower()) || next.isEmpty() || !next.at(0).isUpper()) {
                break;
            }
            product << titleCaseWord(next);
        }
        return product.join(QLatin1Char(' '));
    }
    return {};
}

QString offlineInstallerTitle(const QString& text) {
    const QString lower = text.toLower();
    if (!lower.contains(QStringLiteral("offline installer")) &&
        !(lower.contains(QStringLiteral("offline")) &&
          lower.contains(QStringLiteral("installer")))) {
        return {};
    }

    const QStringList words = meaningfulWords(text);
    const QSet<QString> skip{
        QStringLiteral("create"),
        QStringLiteral("download"),
        QStringLiteral("find"),
        QStringLiteral("get"),
        QStringLiteral("make"),
        QStringLiteral("offline"),
        QStringLiteral("installer"),
        QStringLiteral("installers"),
        QStringLiteral("package"),
        QStringLiteral("packages"),
        QStringLiteral("bundle"),
        QStringLiteral("deployment"),
    };
    const QString product = productNameFromWords(words, skip);
    if (!product.isEmpty()) {
        return boundedTitle(QStringLiteral("%1 Offline Installer").arg(product));
    }
    return QStringLiteral("Offline Installer Download");
}

QString domainTitle(const QString& text) {
    const QString lower = text.toLower();
    struct DomainRule {
        const char* m_title;
        std::initializer_list<const char*> m_terms;
    };
    const std::array<DomainRule, 9> rules = {{
        {.m_title = "AI Panel Quality Pass", .m_terms = {"ai panel", "assistant panel"}},
        {.m_title = "Windows Update Repair", .m_terms = {"windows update"}},
        {.m_title = "BSOD Investigation", .m_terms = {"blue screen", "bsod"}},
        {.m_title = "Malware Cleanup", .m_terms = {"malware", "virus"}},
        {.m_title = "Bloatware Cleanup", .m_terms = {"bloatware", "adware"}},
        {.m_title = "Drive Health Check", .m_terms = {"drive health", "smart check", "smart data"}},
        {.m_title = "Network Connectivity Repair", .m_terms = {"network", "wifi", "wi-fi"}},
        {.m_title = "Printer Troubleshooting", .m_terms = {"printer"}},
        {.m_title = "Partition Manager", .m_terms = {"partition"}},
    }};
    for (const auto& rule : rules) {
        if (containsAnyTerm(lower, rule.m_terms)) {
            return QString::fromLatin1(rule.m_title);
        }
    }
    return offlineInstallerTitle(text);
}

QString genericTitle(const QString& text) {
    QStringList words = meaningfulWords(text);
    if (words.isEmpty()) {
        return {};
    }
    while (words.size() > kMaxTitleWords) {
        words.removeLast();
    }
    QStringList title_words;
    title_words.reserve(words.size());
    for (const auto& word : words) {
        title_words.append(titleCaseWord(word));
    }
    return boundedTitle(title_words.join(QLatin1Char(' ')));
}

bool lowSignalTitle(const QString& title) {
    const QString lower = title.toLower().simplified();
    return title.trimmed().isEmpty() || lower == QLatin1String("run") ||
           lower == QLatin1String("start") || lower == QLatin1String("check") ||
           lower == QLatin1String("fix") || lower == QLatin1String("chat") ||
           lower == QLatin1String("ai chat");
}

}  // namespace

bool isDefaultChatTitle(const QString& title) {
    const QString normalized = title.simplified().toLower();
    return normalized.isEmpty() || normalized == QLatin1String("ai session") ||
           normalized == QLatin1String("ai chat") || normalized == QLatin1String("new chat") ||
           normalized == QLatin1String("untitled");
}

QString chatTitleFromFirstPrompt(const QString& prompt, const QString& workflow_title) {
    const QString clean_prompt = redactedPromptText(prompt);
    QString title = domainTitle(clean_prompt);
    if (title.isEmpty()) {
        title = genericTitle(clean_prompt);
    }

    const QString clean_workflow = redactedPromptText(workflow_title);
    if (lowSignalTitle(title) && !clean_workflow.isEmpty()) {
        title = domainTitle(clean_workflow);
        if (title.isEmpty()) {
            title = genericTitle(clean_workflow);
        }
    }
    if (lowSignalTitle(title)) {
        title = QStringLiteral("AI Chat");
    }
    return boundedTitle(title);
}

}  // namespace sak::ai
