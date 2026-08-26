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
    text.replace(QRegularExpression(QStringLiteral(R"(https?://\S+|www\.\S+)"),
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

bool containsAnyTerm(const QString& lower_text, std::initializer_list<const char*> terms) {
    return std::ranges::any_of(terms, [&](const char* term) {
        return lower_text.contains(QString::fromLatin1(term));
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
        word.remove(QRegularExpression(QStringLiteral(R"([^A-Za-z0-9+\-.])")));
        word = word.trimmed();
        if (word.size() < kMeaningfulWordMinChars) {
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
    for (const QString& word : words) {
        const QString lower_word = word.toLower();
        if (!skip.contains(lower_word)) {
            return boundedTitle(QStringLiteral("%1 Offline Installer").arg(titleCaseWord(word)));
        }
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
