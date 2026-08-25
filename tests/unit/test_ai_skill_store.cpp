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
        // The path stem deliberately DIFFERS from the declared id: renaming a skill file on
        // disk must not re-key the skill, so the front-matter id must win over the stem
        // fallback rather than merely agreeing with it. Every fixture in this file used to
        // pick a stem equal to its own id, so this compare could not tell them apart.
        const Skill skill = Skill::fromMarkdown(bytes, QStringLiteral("/x/renamed-on-disk.md"));

        QVERIFY(skill.isValid());
        QCOMPARE(skill.id, QStringLiteral("package-selection"));
        QCOMPARE(skill.description, QStringLiteral("Map product names to package IDs."));
        QCOMPARE(skill.triggers.size(), 2);
        QCOMPARE(skill.triggers.first(), QStringLiteral("choosing a package"));
        // The body is everything after the closing delimiter, trimmed: the exact QCOMPARE
        // subsumes both the content spot-check and the front-matter-stripped negative check.
        QCOMPARE(skill.body,
                 QStringLiteral("# Package Selection Skill\n\n- Prefer exact vendor IDs."));
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
        // A body whose first lines after the title heading are list items and a
        // sub-heading: '-' bullets, '*' bullets and '#' headings are all rejected as
        // prose (ai_skill.cpp:97-100), so the derived description skips past them to
        // the first real paragraph rather than advertising a bullet to the model.
        const Skill listy = Skill::fromMarkdown(QByteArray("# Drive Health Diagnostics Skill\n"
                                                           "\n"
                                                           "- SMART data.\n"
                                                           "* Event log.\n"
                                                           "## Evidence\n"
                                                           "\n"
                                                           "Collect read-only evidence first.\n"),
                                                QStringLiteral("/x/drive-health-diagnostics.md"));
        QCOMPARE(listy.title, QStringLiteral("Drive Health Diagnostics Skill"));
        QCOMPARE(listy.description, QStringLiteral("Collect read-only evidence first."));
    }

    // A file with no usable body is invalid and is not admitted to the store.
    void fromMarkdown_emptyBodyIsInvalid() {
        const Skill skill = Skill::fromMarkdown(QByteArray("---\nid: empty\n---\n\n"),
                                                QStringLiteral("/x/empty.md"));
        QStringList errors;
        QVERIFY(!skill.isValid(&errors));
        QCOMPARE(errors, QStringList{QStringLiteral("Skill /x/empty.md has an empty body")});
    }

    // The id arm refuses independently of the body arm: a skill with a perfectly good
    // body but no id is still rejected, with its own reason naming the source file.
    void isValid_emptyIdIsInvalidEvenWithBody() {
        Skill nameless;
        nameless.source_path = QStringLiteral("/x/.md");
        nameless.body = QStringLiteral("# T\nbody");
        QStringList errors;
        QVERIFY(!nameless.isValid(&errors));
        QCOMPARE(errors, QStringList{QStringLiteral("Skill /x/.md is missing an id")});

        // A whitespace-only id is treated the same (isValid trims before testing).
        Skill blank_id = nameless;
        blank_id.id = QStringLiteral("   ");
        errors.clear();
        QVERIFY(!blank_id.isValid(&errors));
        QCOMPARE(errors.size(), 1);

        // Both arms accumulate: a skill with neither id nor body reports two reasons.
        Skill empty;
        empty.source_path = QStringLiteral("/x/.md");
        errors.clear();
        QVERIFY(!empty.isValid(&errors));
        QCOMPARE(errors.size(), 2);

        // A file with an empty stem really does reach the store with an empty id,
        // which is exactly what the id arm has to refuse (pins source_path too).
        const Skill parsed = Skill::fromMarkdown(QByteArray("# T\nbody\n"),
                                                 QStringLiteral("/x/.md"));
        QVERIFY(parsed.id.isEmpty());
        SkillStore store;
        errors.clear();
        QVERIFY(!store.addSkill(parsed, &errors));
        QCOMPARE(errors, QStringList{QStringLiteral("Skill /x/.md is missing an id")});
        QCOMPARE(store.size(), 0);
    }

    // loadDirectory reads *.md, and a later skill with the same id overrides the
    // earlier one rather than duplicating.
    void store_loadsDirectoryAndOverridesById() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        // The original alpha carries a trigger so the override can prove stale
        // fields are dropped, not merely overwritten field-by-field.
        writeSkill(temp.path(),
                   QStringLiteral("alpha.md"),
                   "---\nid: alpha\ndescription: first.\nwhen_to_use: stale hint\n---\n"
                   "# Alpha\nbody one\n");
        writeSkill(temp.path(),
                   QStringLiteral("beta.md"),
                   "---\nid: beta\ndescription: second.\n---\n# Beta\nbody two\n");

        SkillStore store;
        QVERIFY(store.loadDirectory(temp.path()));
        QCOMPARE(store.size(), 2);
        const Skill* original = store.skillById(QStringLiteral("alpha"));
        QVERIFY(original != nullptr);
        QCOMPARE(original->description, QStringLiteral("first."));
        QCOMPARE(original->body, QStringLiteral("# Alpha\nbody one"));
        QCOMPARE(original->triggers, QStringList{QStringLiteral("stale hint")});

        // Re-load the same directory with an edited alpha -> the WHOLE record is
        // replaced, not duplicated and not merged field-by-field. The body is what
        // sak_skill "load" serves to the model (ai_assistant_panel.cpp runSkillTool),
        // so a merge that refreshed only the advertised description would leave the
        // model reading stale guidance forever; the dropped trigger pins that the
        // stale fields are cleared rather than retained.
        writeSkill(temp.path(),
                   QStringLiteral("alpha.md"),
                   "---\nid: alpha\ndescription: updated.\n---\n# Alpha Rev\nbody one revised\n");
        QVERIFY(store.loadDirectory(temp.path()));
        QCOMPARE(store.size(), 2);
        const Skill* updated = store.skillById(QStringLiteral("alpha"));
        QVERIFY(updated != nullptr);
        QCOMPARE(updated->description, QStringLiteral("updated."));
        QCOMPARE(updated->title, QStringLiteral("Alpha Rev"));
        QCOMPARE(updated->body, QStringLiteral("# Alpha Rev\nbody one revised"));
        QVERIFY(updated->triggers.isEmpty());
        QCOMPARE(updated->source_path, QDir(temp.path()).filePath(QStringLiteral("alpha.md")));
        // beta is untouched by alpha's override.
        QCOMPARE(store.skillById(QStringLiteral("beta"))->body, QStringLiteral("# Beta\nbody two"));
        QVERIFY(store.skillById(QStringLiteral("missing")) == nullptr);

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
        // Exact key set: the catalog is body-free under EVERY key name, not just "body".
        QCOMPARE(json.keys(),
                 QStringList({QStringLiteral("description"),
                              QStringLiteral("id"),
                              QStringLiteral("title"),
                              QStringLiteral("when_to_use")}));
        const QJsonArray triggers = json.value(QStringLiteral("when_to_use")).toArray();
        QCOMPARE(triggers.size(), 2);
        QCOMPARE(triggers.at(0).toString(), QStringLiteral("a"));
        QCOMPARE(triggers.at(1).toString(), QStringLiteral("b"));
        QVERIFY(!json.contains(QStringLiteral("body")));
    }

    // A leading UTF-8 BOM (written by many Windows editors) must be stripped, or
    // the first-line "---" check fails and front-matter is lost. The strip is
    // LEADING-only: a U+FEFF inside the authored body is content and must survive.
    void fromMarkdown_stripsUtf8Bom() {
        const QByteArray bom = QByteArray::fromHex("EFBBBF");  // UTF-8 BOM
        QByteArray bytes = bom;
        bytes += "---\nid: bommed\ndescription: ok.\nwhen_to_use: a\n---\n# T\nbody";
        bytes += bom;  // interior U+FEFF (ZWNBSP): authored content, not a BOM
        bytes += "text\n";
        const Skill skill = Skill::fromMarkdown(bytes, QStringLiteral("/x/bommed.md"));

        QVERIFY(skill.isValid());
        QCOMPARE(skill.id, QStringLiteral("bommed"));
        QCOMPARE(skill.description, QStringLiteral("ok."));
        QCOMPARE(skill.triggers.size(), 1);
        // Exact body: proves the leading BOM went away AND that no other U+FEFF was
        // collaterally stripped (a blanket text.remove(QChar(kByteOrderMark)) fails here).
        QCOMPARE(skill.body, QStringLiteral("# T\nbody") + QChar(0xFEFF) + QStringLiteral("text"));
    }

    // "triggers" is accepted as an alias for "when_to_use"; each element is trimmed,
    // kept verbatim (never case-folded -- triggers are advertised to the model as
    // authored), and a whitespace-only element is dropped.
    void fromMarkdown_acceptsTriggersAlias() {
        const QByteArray bytes =
            "---\nid: s\ndescription: d.\ntriggers: One, ,two ;  Three\n---\n# T\nbody\n";
        const Skill skill = Skill::fromMarkdown(bytes, QStringLiteral("/x/s.md"));
        // Whole-list QCOMPARE subsumes the size + single-element spot-check. The " "
        // element is dropped only by splitTriggers' !trimmed.isEmpty() guard
        // (Qt::SkipEmptyParts sees " " as non-empty), and "One"/"Three" keep their
        // authored casing.
        const QStringList expected{QStringLiteral("One"),
                                   QStringLiteral("two"),
                                   QStringLiteral("Three")};
        QCOMPARE(skill.triggers, expected);
    }

    // Front-matter opened with "---" but never closed is treated as body, and the
    // id falls back to the file stem (legacy-safe, never crashes).
    void fromMarkdown_unterminatedFrontMatterBecomesBody() {
        const Skill skill =
            Skill::fromMarkdown(QByteArray("---\nid: nope\ndescription: never.\n# Heading\nbody\n"),
                                QStringLiteral("/x/legacy-file.md"));
        QCOMPARE(skill.id, QStringLiteral("legacy-file"));
        // Unterminated front-matter -> the whole (trimmed) input becomes the body verbatim.
        QCOMPARE(skill.body, QStringLiteral("---\nid: nope\ndescription: never.\n# Heading\nbody"));
        QVERIFY(skill.isValid());
    }

    // A user-directory skill overrides a built-in by id (the layering loadDefaults
    // performs), and a new id is added rather than replacing.
    void store_userDirectoryOverridesBuiltInById() {
        SkillStore store;
        QVERIFY(store.loadBuiltIn());
        const int builtin_count = store.size();
        QCOMPARE(builtin_count, 8);  // resources/ai.qrc registers exactly 8 built-in skills

        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        writeSkill(
            temp.path(),
            QStringLiteral("package-selection.md"),
            "---\nid: package-selection\ndescription: user override.\n---\n# X\nuser body\n");
        writeSkill(temp.path(),
                   QStringLiteral("my-custom.md"),
                   "---\nid: my-custom\ndescription: custom.\n---\n# Y\ncustom body\n");

        // skills() order IS the order the system-prompt skill catalog is emitted in, and the
        // order the catalog cap truncates from, so "replaced in place" is a contract, not a
        // comment: an override must keep the built-in's slot instead of teleporting to the tail.
        QStringList expected_ids;
        for (const auto& skill : store.skills()) {
            expected_ids << skill.id;
        }
        QVERIFY(expected_ids.contains(QStringLiteral("package-selection")));

        QVERIFY(store.loadDirectory(temp.path()));
        expected_ids << QStringLiteral("my-custom");  // a new id appends at the tail
        QStringList actual_ids;
        for (const auto& skill : store.skills()) {
            actual_ids << skill.id;
        }
        // Subsumes the count check: the override replaced package-selection IN PLACE and only
        // the custom skill was added.
        QCOMPARE(actual_ids, expected_ids);
        QCOMPARE(store.skillById(QStringLiteral("package-selection"))->description,
                 QStringLiteral("user override."));
        QVERIFY(store.skillById(QStringLiteral("my-custom")) != nullptr);
        // The SERVED body must be the user's, not the built-in resource's: description alone
        // cannot see an override that swapped the advertised blurb while still serving the
        // bundled guidance to the model.
        QCOMPARE(store.skillById(QStringLiteral("package-selection"))->body,
                 QStringLiteral("# X\nuser body"));
        QCOMPARE(store.skillById(QStringLiteral("package-selection"))->title, QStringLiteral("X"));
    }

    // The bundled built-in skills load from the Qt resource and every one is valid.
    void store_loadsBuiltInResources() {
        SkillStore store;
        QStringList errors;
        QVERIFY2(store.loadBuiltIn(&errors), qPrintable(errors.join(QStringLiteral("; "))));
        QCOMPARE(store.size(), 8);  // exactly the 8 qrc-registered built-in skills
        QVERIFY(store.skillById(QStringLiteral("package-selection")) != nullptr);
        QVERIFY(store.skillById(QStringLiteral("malware-removal-triage")) != nullptr);
        // Catalog order is the order the skills are emitted to the model
        // (ai_assistant_panel.cpp:1215) and decides which entries survive the
        // 128-entry truncation, so pin it exactly: SkillStore::loadDirectory
        // enumerates with QDir::Name (ai_skill_store.cpp:81) and addSkill appends
        // in load order, so the built-in order is the qrc file names sorted.
        QStringList ids;
        QStringList missing_when_to_use;
        for (const auto& skill : store.skills()) {
            QVERIFY2(skill.isValid(), qPrintable(skill.id));
            QVERIFY(!skill.description.trimmed().isEmpty());
            ids << skill.id;
            // isValid() checks only id+body, so when_to_use -- the whole basis of
            // progressive disclosure -- needs its own floor.
            if (skill.triggers.isEmpty()) {
                missing_when_to_use << skill.id;
            }
        }
        QCOMPARE(ids,
                 QStringList({QStringLiteral("artifact-verification"),
                              QStringLiteral("cleanup-after-job"),
                              QStringLiteral("customer-handoff-report"),
                              QStringLiteral("drive-health-diagnostics"),
                              QStringLiteral("malware-removal-triage"),
                              QStringLiteral("package-selection"),
                              QStringLiteral("system-cleanup-bloatware-adware"),
                              QStringLiteral("windows-update-repair")}));
        QVERIFY2(missing_when_to_use.isEmpty(),
                 qPrintable(missing_when_to_use.join(QStringLiteral(", "))));
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
