// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_ai_skill_store.cpp
/// @brief Unit tests for the executable skills system (harness Wave 3): Markdown
/// front-matter parsing with legacy fallback, directory loading + id override, and
/// the cheap catalog view used for progressive disclosure.

#include "sak/ai/ai_skill.h"
#include "sak/ai/ai_skill_store.h"

#include <QDir>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace sak::ai {

class AiSkillStoreTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // Front-matter supplies id/description/when-to-use; the body is everything
    // after the closing delimiter.
    void fromMarkdown_parsesFrontMatter() {
        const QByteArray bytes =
            "---\n"
            "id: package-selection\n"
            "description: Map product names to package IDs.\n"
            "when_to_use: choosing a package; ambiguous name\n"
            "---\n"
            "# Package Selection Skill\n"
            "\n"
            "- Prefer exact vendor IDs.\n";
        const Skill skill = Skill::fromMarkdown(bytes, QStringLiteral("/x/package-selection.md"));

        QVERIFY(skill.isValid());
        QCOMPARE(skill.id, QStringLiteral("package-selection"));
        QCOMPARE(skill.description, QStringLiteral("Map product names to package IDs."));
        QCOMPARE(skill.triggers.size(), 2);
        QCOMPARE(skill.triggers.first(), QStringLiteral("choosing a package"));
        QVERIFY(skill.body.contains(QStringLiteral("Prefer exact vendor IDs")));
        QVERIFY(!skill.body.contains(QStringLiteral("description:")));  // front-matter stripped
    }

    // A legacy file with no front-matter still loads: id from the file name, title
    // from the first heading, description from the first paragraph.
    void fromMarkdown_fallsBackWithoutFrontMatter() {
        const QByteArray bytes =
            "# Drive Health Diagnostics Skill\n"
            "\n"
            "Collect read-only evidence first.\n"
            "\n"
            "- SMART data.\n";
        const Skill skill = Skill::fromMarkdown(bytes,
                                                QStringLiteral("/x/drive-health-diagnostics.md"));

        QVERIFY(skill.isValid());
        QCOMPARE(skill.id, QStringLiteral("drive-health-diagnostics"));
        QCOMPARE(skill.title, QStringLiteral("Drive Health Diagnostics Skill"));
        QCOMPARE(skill.description, QStringLiteral("Collect read-only evidence first."));
        QVERIFY(skill.triggers.isEmpty());
    }

    // A file with no usable body is invalid and is not admitted to the store.
    void fromMarkdown_emptyBodyIsInvalid() {
        const Skill skill = Skill::fromMarkdown(QByteArray("---\nid: empty\n---\n\n"),
                                                QStringLiteral("/x/empty.md"));
        QVERIFY(!skill.isValid());
    }

    // loadDirectory reads *.md, and a later skill with the same id overrides the
    // earlier one rather than duplicating.
    void store_loadsDirectoryAndOverridesById() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        writeSkill(temp.path(),
                   QStringLiteral("alpha.md"),
                   "---\nid: alpha\ndescription: first.\n---\n# Alpha\nbody one\n");
        writeSkill(temp.path(),
                   QStringLiteral("beta.md"),
                   "---\nid: beta\ndescription: second.\n---\n# Beta\nbody two\n");

        SkillStore store;
        QVERIFY(store.loadDirectory(temp.path()));
        QCOMPARE(store.size(), 2);
        QVERIFY(store.skillById(QStringLiteral("alpha")) != nullptr);
        QCOMPARE(store.skillById(QStringLiteral("alpha"))->description, QStringLiteral("first."));

        // Re-load the same directory with an updated alpha -> override, not dup.
        writeSkill(temp.path(),
                   QStringLiteral("alpha.md"),
                   "---\nid: alpha\ndescription: updated.\n---\n# Alpha\nbody one\n");
        QVERIFY(store.loadDirectory(temp.path()));
        QCOMPARE(store.size(), 2);
        QCOMPARE(store.skillById(QStringLiteral("alpha"))->description, QStringLiteral("updated."));
        QVERIFY(store.skillById(QStringLiteral("missing")) == nullptr);
    }

    // The catalog view carries id/title/description/when_to_use but never the body.
    void catalogJson_excludesBody() {
        const Skill skill = Skill::fromMarkdown(
            QByteArray("---\nid: s\ndescription: d.\nwhen_to_use: a; b\n---\n# T\nsecret body\n"),
            QStringLiteral("/x/s.md"));
        const QJsonObject json = skill.toCatalogJson();
        QCOMPARE(json.value(QStringLiteral("id")).toString(), QStringLiteral("s"));
        QCOMPARE(json.value(QStringLiteral("description")).toString(), QStringLiteral("d."));
        QCOMPARE(json.value(QStringLiteral("when_to_use")).toArray().size(), 2);
        QVERIFY(!json.contains(QStringLiteral("body")));
    }

    // A leading UTF-8 BOM (written by many Windows editors) must be stripped, or
    // the first-line "---" check fails and front-matter is lost.
    void fromMarkdown_stripsUtf8Bom() {
        QByteArray bytes = QByteArray::fromHex("EFBBBF");  // UTF-8 BOM
        bytes += "---\nid: bommed\ndescription: ok.\nwhen_to_use: a\n---\n# T\nbody text\n";
        const Skill skill = Skill::fromMarkdown(bytes, QStringLiteral("/x/bommed.md"));

        QVERIFY(skill.isValid());
        QCOMPARE(skill.id, QStringLiteral("bommed"));
        QCOMPARE(skill.description, QStringLiteral("ok."));
        QCOMPARE(skill.triggers.size(), 1);
        QVERIFY(!skill.body.contains(QChar(0xFEFF)));
    }

    // "triggers" is accepted as an alias for "when_to_use".
    void fromMarkdown_acceptsTriggersAlias() {
        const Skill skill = Skill::fromMarkdown(
            QByteArray("---\nid: s\ndescription: d.\ntriggers: one, two, three\n---\n# T\nbody\n"),
            QStringLiteral("/x/s.md"));
        QCOMPARE(skill.triggers.size(), 3);
        QCOMPARE(skill.triggers.at(1), QStringLiteral("two"));
    }

    // Front-matter opened with "---" but never closed is treated as body, and the
    // id falls back to the file stem (legacy-safe, never crashes).
    void fromMarkdown_unterminatedFrontMatterBecomesBody() {
        const Skill skill =
            Skill::fromMarkdown(QByteArray("---\nid: nope\ndescription: never.\n# Heading\nbody\n"),
                                QStringLiteral("/x/legacy-file.md"));
        QCOMPARE(skill.id, QStringLiteral("legacy-file"));
        QVERIFY(skill.body.contains(QStringLiteral("id: nope")));
        QVERIFY(skill.isValid());
    }

    // A user-directory skill overrides a built-in by id (the layering loadDefaults
    // performs), and a new id is added rather than replacing.
    void store_userDirectoryOverridesBuiltInById() {
        SkillStore store;
        QVERIFY(store.loadBuiltIn());
        const int builtin_count = store.size();
        QVERIFY(builtin_count >= 8);

        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        writeSkill(
            temp.path(),
            QStringLiteral("package-selection.md"),
            "---\nid: package-selection\ndescription: user override.\n---\n# X\nuser body\n");
        writeSkill(temp.path(),
                   QStringLiteral("my-custom.md"),
                   "---\nid: my-custom\ndescription: custom.\n---\n# Y\ncustom body\n");

        QVERIFY(store.loadDirectory(temp.path()));
        QCOMPARE(store.size(), builtin_count + 1);  // override replaced in place, custom added
        QCOMPARE(store.skillById(QStringLiteral("package-selection"))->description,
                 QStringLiteral("user override."));
        QVERIFY(store.skillById(QStringLiteral("my-custom")) != nullptr);
    }

    // The bundled built-in skills load from the Qt resource and every one is valid.
    void store_loadsBuiltInResources() {
        SkillStore store;
        QStringList errors;
        QVERIFY2(store.loadBuiltIn(&errors), qPrintable(errors.join(QStringLiteral("; "))));
        QVERIFY(store.size() >= 8);
        QVERIFY(store.skillById(QStringLiteral("package-selection")) != nullptr);
        QVERIFY(store.skillById(QStringLiteral("malware-removal-triage")) != nullptr);
        for (const auto& skill : store.skills()) {
            QVERIFY2(skill.isValid(), qPrintable(skill.id));
            QVERIFY(!skill.description.trimmed().isEmpty());
        }
    }

private:
    static void writeSkill(const QString& dir, const QString& name, const QByteArray& contents) {
        QFile file(QDir(dir).filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write(contents);
    }
};

}  // namespace sak::ai

QTEST_GUILESS_MAIN(sak::ai::AiSkillStoreTest)
#include "test_ai_skill_store.moc"
