// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_chat_title.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <initializer_list>

namespace sak::ai {

namespace {

constexpr int kMaxTitleWords = 6;
constexpr qsizetype kUppercaseAcronymMaxChars = 6;
constexpr qsizetype kMeaningfulWordMinChars = 2;

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
    text.replace(QRegularExpression(QStringLiteral(R"(```[\s\S]*?```)")), QStringLiteral(" "));
    text.replace(QRegularExpression(QStringLiteral(R"(`[^`]*`)")), QStringLiteral(" "));
    text.replace(QRegularExpression(QStringLiteral(R"(https?://\S+|www\.\S+)"),
                                    QRegularExpression::CaseInsensitiveOption),
                 QStringLiteral(" website "));
    text.replace(QRegularExpression(QStringLiteral(R"(\b[A-Za-z]:[\\/][^\s]+)")),
                 QStringLiteral(" file "));
    text.replace(QRegularExpression(QStringLiteral(R"(\\\\[^\s]+)")), QStringLiteral(" file "));
    text.replace(QRegularExpression(QStringLiteral(R"(\bsk(?:-proj)?-[A-Za-z0-9_\-]{12,}\b)"),
                                    QRegularExpression::CaseInsensitiveOption),
                 QStringLiteral(" secret "));
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
    return std::any_of(terms.begin(), terms.end(), [&](const char* term) {
        return lower_text.contains(QString::fromLatin1(term));
    });
}

QString titleCaseWord(const QString& word) {
    if (word.isEmpty()) {
        return {};
    }
    const bool keep_upper =
        std::any_of(word.cbegin(), word.cend(), [](QChar ch) { return ch.isDigit(); }) ||
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
    const struct {
        const char* title;
        std::initializer_list<const char*> terms;
    } rules[] = {
        {"AI Panel Quality Pass", {"ai panel", "assistant panel"}},
        {"Windows Update Repair", {"windows update"}},
        {"BSOD Investigation", {"blue screen", "bsod"}},
        {"Malware Cleanup", {"malware", "virus"}},
        {"Bloatware Cleanup", {"bloatware", "adware"}},
        {"Drive Health Check", {"drive health", "smart check", "smart data"}},
        {"Network Connectivity Repair", {"network", "wifi", "wi-fi"}},
        {"Printer Troubleshooting", {"printer"}},
        {"Partition Manager", {"partition"}},
    };
    for (const auto& rule : rules) {
        if (containsAnyTerm(lower, rule.terms)) {
            return QString::fromLatin1(rule.title);
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
