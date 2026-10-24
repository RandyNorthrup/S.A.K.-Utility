"""Pin review-unit coverage and exact source/brief evidence with disposable Git repos."""

import contextlib
import hashlib
import io
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("review_ledger", REPO_ROOT / "scripts/review_ledger.py")
LEDGER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LEDGER)


class ReviewLedgerTests(unittest.TestCase):
    def test_first_party_c_shim_is_reviewable(self):
        self.assertEqual(LEDGER.classify("src/core/sak_lzvn_shim.c"), "src")

    def test_python_test_guards_are_reviewable(self):
        self.assertEqual(LEDGER.classify("tests/unit/test_gate_parity.py"), "tests")
        self.assertEqual(LEDGER.classify("tests/unit/test_mutation_runner.py"), "tests")

    def test_powershell_tests_are_reviewable(self):
        self.assertEqual(LEDGER.classify("tests/unit/test_pack_extension.ps1"), "tests")

    def test_windows_launchers_are_reviewable(self):
        self.assertEqual(LEDGER.classify("scripts/launch_partition_manager_vm_gate.cmd"), "scripts")
        self.assertEqual(LEDGER.classify("scripts/launch.bat"), "scripts")

    def test_c_fixture_is_reviewable(self):
        self.assertEqual(LEDGER.classify("tests/fixtures/cppcheck_c_language_probe.c"), "tests")

    def test_first_party_cmake_root_programs_are_reviewable(self):
        for path in ("CMakeLists.txt", "tests/CMakeLists.txt"):
            with self.subTest(path=path):
                self.assertEqual(LEDGER.classify(path), "cmake")

    def test_first_party_cmake_modules_are_reviewable(self):
        for path in ("cmake/copy_tools_bundle.cmake", "cmake/SakSourceFingerprint.cmake",
                     "cmake/helpers/example.cmake"):
            with self.subTest(path=path):
                self.assertEqual(LEDGER.classify(path), "cmake")

    def test_generated_and_vendor_cmake_programs_stay_excluded(self):
        for path in ("build/CMakeLists.txt", "build/cmake/generated.cmake",
                     "third_party/CMakeLists.txt", "vendor/cmake/vendor.cmake",
                     "src/CMakeLists.txt", "cmake/generated.cpp"):
            with self.subTest(path=path):
                self.assertIsNone(LEDGER.classify(path))

    def test_displayed_rules_follow_declared_prefix_and_exact_rules(self):
        with mock.patch.dict(LEDGER.UNIT_RULES, {"probe": (".probe",)}), \
                mock.patch.dict(LEDGER.UNIT_FILE_RULES, {"BuildProbe.txt": "probe"}):
            description = LEDGER.unit_rule_description()
            self.assertIn("probe/**.probe", description)
            self.assertIn("BuildProbe.txt=probe", description)
            self.assertEqual(LEDGER.classify("BuildProbe.txt"), "probe")
        self.assertIn("cmake/**.cmake", LEDGER.unit_rule_description())
        self.assertIn("CMakeLists.txt=cmake", LEDGER.unit_rule_description())
        self.assertIn("tests/CMakeLists.txt=cmake", LEDGER.unit_rule_description())

    def test_existing_languages_remain_reviewable(self):
        paths = {
            "include/sak/file_hash.h": "include", "include/sak/types.hpp": "include",
            "src/core/file_hash.cpp": "src", "tests/unit/test_file_hash.cpp": "tests",
            "tests/unit/test_browser.mjs": "tests", "scripts/run_mutation_test.py": "scripts",
            "scripts/check_doc_accuracy.ps1": "scripts", "browser/extension/background.js": "browser",
        }
        for path, group in paths.items():
            with self.subTest(path=path):
                self.assertEqual(LEDGER.classify(path), group)

    def test_resources_and_generated_output_are_not_source_units(self):
        for path in ("docs/future/PLAN.md", "build/generated.cpp", "temp/probe.py",
                     "resources/workflow.json", "third_party/vendor.c", "tests/README.md",
                     "src_snapshot/old.cpp"):
            with self.subTest(path=path):
                self.assertIsNone(LEDGER.classify(path))


class ReviewLedgerIntegrityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        template = tempfile.TemporaryDirectory(prefix="sak-ledger-template-")
        cls.addClassCleanup(template.cleanup)
        cls.template_root = Path(template.name) / "repo"
        cls.template_root.mkdir()
        (cls.template_root / "src").mkdir()
        (cls.template_root / "scripts").mkdir()
        (cls.template_root / "docs/review_briefs").mkdir(parents=True)
        (cls.template_root / "src/sample.cpp").write_bytes(b"int value = 1;\r\n")
        (cls.template_root / "docs/review_briefs/sample.md").write_bytes(b"NO FINDINGS\r\n")
        (cls.template_root / ".gitattributes").write_text(
            "*.cpp text eol=lf\n*.md text eol=lf\n", encoding="ascii")
        for arguments in (
            ("init", "--quiet"),
            ("config", "user.name", "Ledger Test"),
            ("config", "user.email", "test@example.invalid"),
            ("config", "core.autocrlf", "false"),
            ("add", ".gitattributes", "src/sample.cpp", "docs/review_briefs/sample.md"),
            ("-c", "commit.gpgsign=false", "commit", "--quiet", "-m", "fixture"),
        ):
            subprocess.run(["git", *arguments], cwd=cls.template_root,
                           capture_output=True, text=True, encoding="utf-8", check=True)

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="sak-ledger-test-")
        self.addCleanup(temporary.cleanup)
        self.fixture_root = Path(temporary.name)
        self.root = self.fixture_root / "repo"
        # Every case still owns a separate repo; only its identical clean base is copied.
        shutil.copytree(self.template_root, self.root)
        # The copied index still holds the template paths' stat cache. Refresh it before
        # tests that intentionally hide a source edit with assume-unchanged/skip-worktree.
        self.git("update-index", "--refresh")
        self.source = self.root / "src/sample.cpp"
        self.brief = self.root / "docs/review_briefs/sample.md"
        self.state_path = self.root / "scripts/review_ledger_state.json"
        patcher = mock.patch.multiple(
            LEDGER, REPO_ROOT=self.root, STATE_PATH=self.state_path,
            REMEDIATION_DOC=self.root / "docs/CODEX_REVIEW_5_REMEDIATION.md")
        patcher.start()
        self.addCleanup(patcher.stop)

    def git(self, *arguments):
        result = subprocess.run(
            ["git", *arguments], cwd=self.root, capture_output=True, text=True,
            encoding="utf-8", check=True)
        return result.stdout.strip()

    def call(self, function, *arguments, **keywords):
        with contextlib.redirect_stdout(io.StringIO()):
            return function(*arguments, **keywords)

    def state(self):
        return json.loads(self.state_path.read_text(encoding="utf-8"))

    def mark_source(self):
        self.call(LEDGER.cmd_inventory, quiet=True)
        blob = self.git("hash-object", "--path=src/sample.cpp", "--", "src/sample.cpp")
        self.call(LEDGER.cmd_mark, "src/sample.cpp", "docs/review_briefs/sample.md", blob)
        return blob

    def test_assume_unchanged_cannot_hide_changed_review_bytes(self):
        self.mark_source()
        self.git("update-index", "--assume-unchanged", "src/sample.cpp")
        self.source.write_bytes(b"int value = 2;\r\n")
        self.assertEqual(self.git("diff-files", "--name-only"), "")
        expected = self.git("hash-object", "--path=src/sample.cpp", "--", "src/sample.cpp")
        self.assertEqual(LEDGER.current_units()["src/sample.cpp"]["blob"], expected)
        self.call(LEDGER.cmd_inventory, quiet=True)
        self.assertEqual(self.state()["units"]["src/sample.cpp"]["status"], "pending")

    def test_skip_worktree_cannot_hide_changed_review_bytes(self):
        self.mark_source()
        self.git("update-index", "--skip-worktree", "src/sample.cpp")
        self.source.write_bytes(b"int value = 3;\r\n")
        self.assertEqual(self.git("diff-files", "--name-only"), "")
        expected = self.git("hash-object", "--path=src/sample.cpp", "--", "src/sample.cpp")
        self.assertEqual(LEDGER.current_units()["src/sample.cpp"]["blob"], expected)

    def test_empty_brief_cannot_be_marked(self):
        self.call(LEDGER.cmd_inventory, quiet=True)
        self.brief.write_text(" \n\t", encoding="ascii")
        before = self.state_path.read_bytes()
        with self.assertRaises(SystemExit):
            self.call(LEDGER.cmd_mark, "src/sample.cpp", "docs/review_briefs/sample.md",
                      LEDGER.working_tree_blob("src/sample.cpp"))
        self.assertEqual(self.state_path.read_bytes(), before)

    def test_missing_brief_invalidates_review(self):
        self.mark_source()
        self.brief.unlink()
        self.call(LEDGER.cmd_inventory, quiet=True)
        self.assertEqual(self.state()["units"]["src/sample.cpp"]["status"], "pending")

    def test_changed_brief_invalidates_review(self):
        self.mark_source()
        self.brief.write_text("Different review evidence\n", encoding="ascii")
        self.call(LEDGER.cmd_inventory, quiet=True)
        self.assertEqual(self.state()["units"]["src/sample.cpp"]["status"], "pending")

    def test_explicit_archived_binding_keeps_exact_review_credit(self):
        self.mark_source()
        archived = self.root / "docs/archive/review_briefs/sample.md"
        archived.parent.mkdir(parents=True)
        self.brief.rename(archived)
        state = self.state()
        state["units"]["src/sample.cpp"]["brief"] = archived.relative_to(self.root).as_posix()
        self.state_path.write_text(json.dumps(state), encoding="utf-8")
        self.call(LEDGER.cmd_inventory, quiet=True)
        self.assertEqual(self.state()["units"]["src/sample.cpp"]["status"], "reviewed",
                         "Explicit exact archived brief lost existing review credit")

    def test_external_brief_is_never_rewritten(self):
        self.call(LEDGER.cmd_inventory, quiet=True)
        outside = self.fixture_root / "outside-review.md"
        outside.write_text("Review \u2014 text", encoding="utf-8")
        before = outside.read_bytes()
        with self.assertRaises(SystemExit):
            self.call(LEDGER.cmd_mark, "not-a-unit", "../outside-review.md",
                      LEDGER.working_tree_blob("src/sample.cpp"))
        self.assertEqual(outside.read_bytes(), before)

    def test_stale_source_refusal_does_not_normalize_brief(self):
        self.call(LEDGER.cmd_inventory, quiet=True)
        expected = LEDGER.working_tree_blob("src/sample.cpp")
        self.source.write_text("int changed = 1;\n", encoding="ascii")
        self.brief.write_text("Review \u2014 text", encoding="utf-8")
        before = self.brief.read_bytes()
        with self.assertRaises(SystemExit):
            self.call(LEDGER.cmd_mark, "src/sample.cpp", "docs/review_briefs/sample.md", expected)
        self.assertEqual(self.brief.read_bytes(), before)

    def assert_mark_intervening_edit_refused(self, target):
        self.call(LEDGER.cmd_inventory, quiet=True)
        expected = LEDGER.working_tree_blob("src/sample.cpp")
        before = self.state_path.read_bytes()
        original_hash = LEDGER.brief_blob
        def edit_after_hash(*arguments):
            digest = original_hash(*arguments)
            target.write_bytes(b"Intervening edit\n")
            return digest
        with mock.patch.object(LEDGER, "brief_blob", side_effect=edit_after_hash):
            with self.assertRaises(SystemExit):
                self.call(LEDGER.cmd_mark, "src/sample.cpp", "docs/review_briefs/sample.md", expected)
        self.assertEqual(self.state_path.read_bytes(), before)

    def test_mark_rechecks_source_after_brief_admission(self):
        self.assert_mark_intervening_edit_refused(self.source)

    def test_mark_rechecks_brief_after_hashing(self):
        self.assert_mark_intervening_edit_refused(self.brief)

    def test_invalid_selection_refuses_before_inventory_writes(self):
        for count, group in ((-1, ""), (0, ""), (1, "scripst")):
            with self.subTest(count=count, group=group):
                with mock.patch.object(LEDGER, "cmd_inventory", wraps=LEDGER.cmd_inventory) as scan:
                    with self.assertRaises(SystemExit):
                        self.call(LEDGER.cmd_next, count, group)
                    scan.assert_not_called()

    def test_valid_next_keeps_driver_output_shape(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(LEDGER.cmd_next(1, "src"), 0)
        self.assertEqual(output.getvalue(), "1 pending; next 1:\n  src/sample.cpp\n")

    def test_new_cmake_units_start_pending_without_review_credit(self):
        self.mark_source()
        paths = ("CMakeLists.txt", "tests/CMakeLists.txt", "cmake/copy_tools_bundle.cmake",
                 "cmake/SakSourceFingerprint.cmake")
        for relative in paths:
            target = self.root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("# fixture build program\n", encoding="ascii")
            self.git("add", "--", relative)
        self.call(LEDGER.cmd_inventory, quiet=True)
        state = self.state()
        for path in paths:
            self.assertEqual(state["units"][path], {"group": "cmake", "status": "pending"})
        self.assertEqual(state["units"]["src/sample.cpp"]["status"], "reviewed")
        self.assertEqual(state["unit_rule"], LEDGER.unit_rule_description())

    def test_batch_hashes_match_individual_filters_and_non_ascii_paths(self):
        for relative in ("src/with space.cpp", "src/caf\u00e9.cpp"):
            (self.root / relative).write_bytes(b"int value = 2;\r\n")
            self.git("add", "--", relative)
        units = LEDGER.current_units()
        self.assertEqual(len(units), 3)
        for relative, meta in units.items():
            self.assertEqual(meta["blob"], self.git("hash-object", f"--path={relative}",
                                                    "--", relative))
            self.assertNotEqual(meta["blob"], self.git("hash-object", "--no-filters", "--", relative))

    def test_batch_hash_count_and_format_fail_closed(self):
        original_git = LEDGER.git
        for output in ("", "a" * 40 + "\n" + "b" * 40 + "\n", "g" * 40 + "\n"):
            with self.subTest(output=output):
                def fake_git(*arguments, **keywords):
                    if arguments == ("hash-object", "--stdin-paths"):
                        return output
                    return original_git(*arguments, **keywords)
                with mock.patch.object(LEDGER, "git", side_effect=fake_git):
                    with self.assertRaises(SystemExit):
                        self.call(LEDGER.current_units)

    def test_untracked_owned_brief_remains_compatible_with_driver(self):
        self.call(LEDGER.cmd_inventory, quiet=True)
        relative = "docs/review_briefs/new-review.md"
        (self.root / relative).write_text("NO FINDINGS\n", encoding="ascii")
        self.call(LEDGER.cmd_mark, "src/sample.cpp", relative,
                  LEDGER.working_tree_blob("src/sample.cpp"))
        entry = self.state()["units"]["src/sample.cpp"]
        self.assertEqual(entry["brief"], relative)
        self.assertEqual(entry["reviewed_brief_blob"], LEDGER.working_tree_blob(relative))

    def test_normalization_is_bound_to_stored_brief_bytes(self):
        self.brief.write_bytes("Review \u2014 clear\n".encode("utf-8"))
        self.mark_source()
        self.assertEqual(self.brief.read_bytes(), b"Review -- clear\n")
        entry = self.state()["units"]["src/sample.cpp"]
        self.assertEqual(entry["reviewed_brief_blob"],
                         LEDGER.working_tree_blob("docs/review_briefs/sample.md"))
        self.call(LEDGER.cmd_inventory, quiet=True)
        self.assertEqual(self.state()["units"]["src/sample.cpp"]["status"], "reviewed")

    def test_invalid_brief_bytes_refuse_without_writes(self):
        self.call(LEDGER.cmd_inventory, quiet=True)
        state_before = self.state_path.read_bytes()
        for payload in (b"\xff", b"NO\x00FINDINGS", "Unknown \U0001f680".encode("utf-8"),
                        b"A" * (LEDGER.MAX_BRIEF_BYTES + 1)):
            with self.subTest(size=len(payload)):
                self.brief.write_bytes(payload)
                with self.assertRaises(SystemExit):
                    self.call(LEDGER.cmd_mark, "src/sample.cpp", "docs/review_briefs/sample.md",
                              LEDGER.working_tree_blob("src/sample.cpp"))
                self.assertEqual(self.brief.read_bytes(), payload)
                self.assertEqual(self.state_path.read_bytes(), state_before)

    def test_normalization_cannot_create_empty_evidence(self):
        self.call(LEDGER.cmd_inventory, quiet=True)
        self.brief.write_text(str(self.root), encoding="utf-8")
        before = self.brief.read_bytes()
        with self.assertRaises(SystemExit):
            self.call(LEDGER.cmd_mark, "src/sample.cpp", "docs/review_briefs/sample.md",
                      LEDGER.working_tree_blob("src/sample.cpp"))
        self.assertEqual(self.brief.read_bytes(), before)

    def test_invalid_brief_paths_refuse_without_writes(self):
        self.call(LEDGER.cmd_inventory, quiet=True)
        (self.root / "src/prose.md").write_text("Review \u2014 text", encoding="utf-8")
        (self.root / "docs/review_briefs/prose.cpp").write_text("Review \u2014 text", encoding="utf-8")
        state_before = self.state_path.read_bytes()
        for relative in (str(self.brief), "src/prose.md", "docs/review_briefs/prose.cpp",
                         "docs/review_briefs/../review_briefs/sample.md",
                         "docs/review_briefs/sample.md:stream"):
            with self.subTest(path=relative):
                with self.assertRaises(SystemExit):
                    self.call(LEDGER.cmd_mark, "src/sample.cpp", relative,
                              LEDGER.working_tree_blob("src/sample.cpp"))
                self.assertEqual(self.state_path.read_bytes(), state_before)

    def test_windows_junction_cannot_escape_brief_root(self):
        outside = self.fixture_root / "outside"
        outside.mkdir()
        evidence = outside / "review.md"
        evidence.write_text("Review \u2014 text", encoding="utf-8")
        link = self.root / "docs/review_briefs/escape"
        subprocess.run(["cmd.exe", "/d", "/c", "mklink", "/J", str(link), str(outside)],
                       capture_output=True, check=True, creationflags=subprocess.CREATE_NO_WINDOW)
        self.addCleanup(link.rmdir)
        self.call(LEDGER.cmd_inventory, quiet=True)
        before = evidence.read_bytes()
        with self.assertRaises(SystemExit):
            self.call(LEDGER.cmd_mark, "src/sample.cpp", "docs/review_briefs/escape/review.md",
                      LEDGER.working_tree_blob("src/sample.cpp"))
        self.assertEqual(evidence.read_bytes(), before)

    def test_invalidation_preserves_history_across_refresh(self):
        original_blob = self.mark_source()
        original_brief_blob = self.state()["units"]["src/sample.cpp"]["reviewed_brief_blob"]
        self.brief.write_text("Changed review\n", encoding="ascii")
        for _ in range(2):
            self.call(LEDGER.cmd_inventory, quiet=True)
        entry = self.state()["units"]["src/sample.cpp"]
        self.assertEqual(entry["status"], "pending")
        self.assertEqual(entry["previous_review_blob"], original_blob)
        self.assertEqual(entry["previous_evidence"], "driver")
        self.assertEqual(entry["previous_brief"], "docs/review_briefs/sample.md")
        self.assertEqual(entry["previous_brief_blob"], original_brief_blob)

    def test_failed_atomic_state_replace_preserves_prior_bytes(self):
        self.call(LEDGER.cmd_inventory, quiet=True)
        before = self.state_path.read_bytes()
        with mock.patch.object(LEDGER.os, "replace", side_effect=OSError("injected replace failure")):
            with self.assertRaises(SystemExit):
                self.call(LEDGER.save_state, {"units": {}})
        self.assertEqual(self.state_path.read_bytes(), before)
        self.assertEqual(list(self.state_path.parent.glob(".review-ledger-*")), [])

    def test_invalid_state_schema_and_bindings_refuse_without_writes(self):
        bad_states = [[], {"units": []}, {"units": {}, "schema_version": True},
                      {"units": {"src/sample.cpp": {"status": "unknown"}}},
                      {"units": {"src/sample.cpp": {"status": "reviewed", "evidence": "guessed"}}},
                      {"units": {"src/sample.cpp": {"status": "pending", "reviewed_blob": "a" * 41}}}]
        for state in bad_states:
            with self.subTest(state=state):
                self.state_path.write_text(json.dumps(state), encoding="utf-8")
                before = self.state_path.read_bytes()
                with self.assertRaises(SystemExit):
                    self.call(LEDGER.cmd_inventory, quiet=True)
                self.assertEqual(self.state_path.read_bytes(), before)

    def legacy_manifest(self, *, second_unit=False):
        self.mark_source()
        if second_unit:
            (self.root / "src/second.cpp").write_text("int second = 1;\n", encoding="ascii")
            (self.root / "docs/review_briefs/second.md").write_text("NO FINDINGS\n", encoding="ascii")
            self.git("add", "src/second.cpp", "docs/review_briefs/second.md")
            self.call(LEDGER.cmd_inventory, quiet=True)
            self.call(LEDGER.cmd_mark, "src/second.cpp", "docs/review_briefs/second.md",
                      LEDGER.working_tree_blob("src/second.cpp"))
        state = self.state()
        entries = []
        for path, meta in state["units"].items():
            entries.append({"path": path, "reviewed_blob": meta["reviewed_blob"],
                            "brief": meta["brief"], "expected_brief_blob": meta["reviewed_brief_blob"]})
            del meta["reviewed_brief_blob"]
        state.pop("schema_version", None)
        self.state_path.write_text(json.dumps(state), encoding="utf-8")
        manifest = {"schema_version": 1,
                    "state_sha256": hashlib.sha256(self.state_path.read_bytes()).hexdigest(),
                    "entries": entries}
        return state, manifest

    def write_manifest(self, manifest):
        path = self.fixture_root / "migration.json"
        path.write_text(json.dumps(manifest), encoding="utf-8")
        return str(path)

    def assert_migration_refuses_without_writes(self, manifest):
        before = self.state_path.read_bytes()
        with self.assertRaises(SystemExit):
            self.call(LEDGER.cmd_migrate_brief_bindings, self.write_manifest(manifest))
        self.assertEqual(self.state_path.read_bytes(), before)

    def test_legacy_driver_evidence_requires_explicit_migration(self):
        self.legacy_manifest()
        before = self.state_path.read_bytes()
        with self.assertRaises(SystemExit):
            self.call(LEDGER.cmd_inventory, quiet=True)
        self.assertEqual(self.state_path.read_bytes(), before)

    def test_explicit_migration_preserves_existing_review_credit(self):
        prior, manifest = self.legacy_manifest(second_unit=True)
        manifest_path = self.write_manifest(manifest)
        self.call(LEDGER.cmd_migrate_brief_bindings, manifest_path)
        state = self.state()
        self.assertEqual(state["schema_version"], 2)
        self.assertEqual(set(state["units"]), set(prior["units"]))
        for entry in manifest["entries"]:
            updated = state["units"][entry["path"]]
            self.assertEqual({key: updated[key] for key in prior["units"][entry["path"]]},
                             prior["units"][entry["path"]])
            self.assertEqual(updated["reviewed_brief_blob"], entry["expected_brief_blob"])
        self.assertEqual(state["brief_binding_migration"]["original_state_sha256"],
                         manifest["state_sha256"])
        self.call(LEDGER.cmd_inventory, quiet=True)
        self.assertTrue(all(meta["status"] == "reviewed" for meta in self.state()["units"].values()))
        self.assert_migration_refuses_without_writes(manifest)

    def test_migration_refuses_changed_state(self):
        _, manifest = self.legacy_manifest()
        manifest["state_sha256"] = "0" * 64
        self.assert_migration_refuses_without_writes(manifest)

    def test_migration_rejects_invalid_manifest_schema_and_entry_fields(self):
        _, manifest = self.legacy_manifest()
        invalid = [{**manifest, "schema_version": True}, {**manifest, "extra": 1},
                   {**manifest, "entries": [{**manifest["entries"][0], "unexpected": 1}]},
                   {**manifest, "entries": [{**manifest["entries"][0], "brief": 1}]}]
        for candidate in invalid:
            with self.subTest(manifest=candidate):
                self.assert_migration_refuses_without_writes(candidate)

    def test_migration_rejects_oversized_manifest(self):
        _, manifest = self.legacy_manifest()
        path = self.fixture_root / "oversized-manifest.json"
        content = json.dumps(manifest).encode("utf-8")
        # Valid JSON plus whitespace stays parseable even after the bounded read. This isolates
        # the byte cap: invalid JSON would still refuse with the cap deliberately removed.
        path.write_bytes(content + b" " * (LEDGER.MAX_MIGRATION_MANIFEST_BYTES + 1 - len(content)))
        before = self.state_path.read_bytes()
        with self.assertRaises(SystemExit):
            self.call(LEDGER.cmd_migrate_brief_bindings, str(path))
        self.assertEqual(self.state_path.read_bytes(), before)

    def test_migration_requires_exact_legacy_entry_set(self):
        _, original = self.legacy_manifest()
        for entries in ([], original["entries"] * 2,
                        original["entries"] + [{**original["entries"][0], "path": "src/extra.cpp"}]):
            with self.subTest(entries=len(entries)):
                self.assert_migration_refuses_without_writes({**original, "entries": entries})

    def test_migration_refuses_changed_live_source(self):
        _, manifest = self.legacy_manifest()
        self.source.write_text("int value = 7;\n", encoding="ascii")
        self.assert_migration_refuses_without_writes(manifest)

    def test_migration_refuses_substituted_source_review(self):
        _, manifest = self.legacy_manifest()
        manifest["entries"][0]["reviewed_blob"] = "0" * 40
        self.assert_migration_refuses_without_writes(manifest)

    def test_migration_cannot_relabel_changed_source_as_old_review(self):
        _, manifest = self.legacy_manifest()
        self.source.write_text("int new_review_needed = 1;\n", encoding="ascii")
        manifest["entries"][0]["reviewed_blob"] = LEDGER.working_tree_blob("src/sample.cpp")
        self.assert_migration_refuses_without_writes(manifest)

    def test_migration_refuses_substituted_brief_path(self):
        _, manifest = self.legacy_manifest()
        alternate = "docs/review_briefs/alternate.md"
        (self.root / alternate).write_bytes(self.brief.read_bytes())
        manifest["entries"][0]["brief"] = alternate
        self.assert_migration_refuses_without_writes(manifest)

    def test_migration_refuses_unconfirmed_brief_hash(self):
        _, manifest = self.legacy_manifest()
        manifest["entries"][0]["expected_brief_blob"] = "0" * 40
        self.assert_migration_refuses_without_writes(manifest)

    def test_migration_refuses_changed_brief(self):
        _, manifest = self.legacy_manifest()
        self.brief.write_text("Different review\n", encoding="ascii")
        self.assert_migration_refuses_without_writes(manifest)

    def test_migration_rejects_empty_brief_even_with_matching_confirmation(self):
        _, manifest = self.legacy_manifest()
        self.brief.write_bytes(b"")
        manifest["entries"][0]["expected_brief_blob"] = LEDGER.working_tree_blob(
            "docs/review_briefs/sample.md")
        self.assert_migration_refuses_without_writes(manifest)

    def test_migration_checks_every_entry_before_writing(self):
        _, manifest = self.legacy_manifest(second_unit=True)
        manifest["entries"][-1]["expected_brief_blob"] = "0" * 40
        self.assert_migration_refuses_without_writes(manifest)

    def test_migration_never_normalizes_confirmed_evidence(self):
        _, manifest = self.legacy_manifest()
        self.brief.write_text("Review \u2014 text\n", encoding="utf-8")
        manifest["entries"][0]["expected_brief_blob"] = LEDGER.working_tree_blob(
            "docs/review_briefs/sample.md")
        before = self.brief.read_bytes()
        self.assert_migration_refuses_without_writes(manifest)
        self.assertEqual(self.brief.read_bytes(), before)

    def test_migration_rechecks_sources_after_all_brief_admissions(self):
        _, manifest = self.legacy_manifest()
        original_confirm = LEDGER.confirm_migration_entry
        call_count = 0
        def mutate_after_first_admission(*arguments):
            nonlocal call_count
            digest = original_confirm(*arguments)
            call_count += 1
            if call_count == 1:
                self.source.write_text("int late_change = 1;\n", encoding="ascii")
            return digest
        with mock.patch.object(LEDGER, "confirm_migration_entry", side_effect=mutate_after_first_admission):
            self.assert_migration_refuses_without_writes(manifest)

    def test_migration_refuses_state_changed_during_admission(self):
        _, manifest = self.legacy_manifest()
        original_confirm = LEDGER.confirm_migration_entry
        def change_state(*arguments):
            digest = original_confirm(*arguments)
            state = self.state()
            state["intervening_edit"] = True
            self.state_path.write_text(json.dumps(state), encoding="utf-8")
            return digest
        with mock.patch.object(LEDGER, "confirm_migration_entry", side_effect=change_state):
            with self.assertRaises(SystemExit):
                self.call(LEDGER.cmd_migrate_brief_bindings, self.write_manifest(manifest))
        self.assertTrue(self.state()["intervening_edit"])
        self.assertNotIn("reviewed_brief_blob", self.state()["units"]["src/sample.cpp"])

    def relocation_manifest(self, *, second_unit=False, destination_suffix=""):
        self.mark_source()
        if second_unit:
            (self.root / "src/second.cpp").write_bytes(b"int second = 1;\n")
            (self.root / "docs/review_briefs/second.md").write_bytes(b"Second review\r\n")
            self.git("add", "src/second.cpp")
            self.call(LEDGER.cmd_inventory, quiet=True)
            self.call(LEDGER.cmd_mark, "src/second.cpp", "docs/review_briefs/second.md",
                      LEDGER.working_tree_blob("src/second.cpp"))
        prior = self.state()
        prior["retained_history"] = {"checkpoint": "unchanged"}
        prior["units"]["src/sample.cpp"]["previous_evidence"] = "cited"
        self.state_path.write_text(json.dumps(prior), encoding="utf-8")
        entries = []
        for path, meta in prior["units"].items():
            old = self.root / meta["brief"]
            archived = "docs/archive/review_briefs/" + old.stem + destination_suffix + old.suffix
            entries.append({"path": path, "reviewed_blob": meta["reviewed_blob"],
                            "brief": meta["brief"],
                            "reviewed_brief_blob": meta["reviewed_brief_blob"],
                            "brief_sha256": hashlib.sha256(old.read_bytes()).hexdigest(),
                            "archived_brief": archived})
            target = self.root / archived
            target.parent.mkdir(parents=True, exist_ok=True)
            old.rename(target)
        return prior, {"schema_version": 1,
                       "state_sha256": hashlib.sha256(self.state_path.read_bytes()).hexdigest(),
                       "entries": entries}

    def relocation_files(self):
        return {path.relative_to(self.root).as_posix(): path.read_bytes()
                for folder in ("src", "docs") for path in (self.root / folder).rglob("*")
                if path.is_file()}

    def assert_relocation_refuses(self, manifest):
        before = self.state_path.read_bytes()
        files = self.relocation_files()
        manifest_path = self.write_manifest(manifest)
        manifest_bytes = Path(manifest_path).read_bytes()
        with self.assertRaises(SystemExit):
            self.call(LEDGER.cmd_relocate_briefs, manifest_path)
        self.assertEqual(self.state_path.read_bytes(), before, "Refusal changed ledger bytes")
        self.assertEqual(self.relocation_files(), files, "Relocation changed source/brief files")
        self.assertEqual(Path(manifest_path).read_bytes(), manifest_bytes)

    def test_archive_relocation_preserves_credit_history_and_unselected_units(self):
        prior, manifest = self.relocation_manifest(second_unit=True)
        unselected = manifest["entries"].pop()
        (self.root / unselected["archived_brief"]).rename(self.root / unselected["brief"])
        files = self.relocation_files()
        manifest_path = self.write_manifest(manifest)
        self.assertEqual(self.call(LEDGER.cmd_relocate_briefs, manifest_path), 0)
        state = self.state()
        entry = manifest["entries"][0]
        expected = json.loads(json.dumps(prior))
        selected = expected["units"][entry["path"]]
        selected["brief"] = entry["archived_brief"]
        selected["reviewed_brief_blob"] = self.git("hash-object",
                                                 "--path=" + entry["archived_brief"],
                                                 "--", entry["archived_brief"])
        expected["brief_relocations"] = [{
            "original_state_sha256": manifest["state_sha256"],
            "manifest_sha256": hashlib.sha256(Path(manifest_path).read_bytes()).hexdigest(),
            "relocated_units": [entry["path"]],
        }]
        self.assertEqual(state, expected, "Relocation altered unrelated credit/history metadata")
        self.assertEqual(self.relocation_files(), files, "Relocation rewrote owner-moved files")
        self.call(LEDGER.cmd_inventory, quiet=True)
        self.assertEqual(self.state()["units"], expected["units"])
        self.assert_relocation_refuses(manifest)

    def test_archive_relocation_uses_new_path_git_filters_without_normalizing(self):
        with (self.root / ".gitattributes").open("a", encoding="ascii") as stream:
            stream.write("docs/archive/** -text\n")
        _, manifest = self.relocation_manifest()
        entry = manifest["entries"][0]
        target = self.root / entry["archived_brief"]
        raw = target.read_bytes()
        expected = self.git("hash-object", "--no-filters", "--", entry["archived_brief"])
        self.assertNotEqual(expected, entry["reviewed_brief_blob"])
        self.call(LEDGER.cmd_relocate_briefs, self.write_manifest(manifest))
        self.assertEqual(self.state()["units"][entry["path"]]["reviewed_brief_blob"], expected,
                         "Relocation retained the old-path filtered blob")
        self.assertEqual(target.read_bytes(), raw, "Relocation normalized archived bytes")
        self.call(LEDGER.cmd_inventory, quiet=True)
        self.assertEqual(self.state()["units"][entry["path"]]["status"], "reviewed")

    def test_archive_relocation_accepts_removed_empty_active_directory(self):
        _, manifest = self.relocation_manifest()
        active = self.root / "docs/review_briefs"
        active.rmdir()
        self.assertFalse(active.exists())
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            try:
                result = LEDGER.cmd_relocate_briefs(self.write_manifest(manifest))
            except SystemExit:
                self.fail("Absent old brief directory was refused: " + output.getvalue())
        self.assertEqual(result, 0)
        self.assertFalse(active.exists(), "Metadata relocation recreated the absent active root")
        self.call(LEDGER.cmd_inventory, quiet=True)
        entry = self.state()["units"]["src/sample.cpp"]
        self.assertEqual(entry["status"], "reviewed")
        self.assertEqual(entry["brief"], "docs/archive/review_briefs/sample.md")

    def test_archive_relocation_missing_old_path_still_refuses_junction_escape(self):
        _, manifest = self.relocation_manifest()
        outside = self.root / "outside-active"
        outside.mkdir()
        link = self.root / "docs/review_briefs/escape"
        subprocess.run(["cmd.exe", "/d", "/c", "mklink", "/J", str(link), str(outside)],
                       capture_output=True, check=True, creationflags=subprocess.CREATE_NO_WINDOW)
        self.addCleanup(link.rmdir)
        entry = manifest["entries"][0]
        entry["brief"] = "docs/review_briefs/escape/sample.md"
        state = self.state()
        state["units"][entry["path"]]["brief"] = entry["brief"]
        self.state_path.write_text(json.dumps(state), encoding="utf-8")
        manifest["state_sha256"] = hashlib.sha256(self.state_path.read_bytes()).hexdigest()
        self.assertFalse((self.root / entry["brief"]).exists())
        self.assert_relocation_refuses(manifest)
        self.assertEqual(list(outside.iterdir()), [])

    def test_archive_reader_never_searches_for_missing_active_path(self):
        self.relocation_manifest()
        self.call(LEDGER.cmd_inventory, quiet=True)
        entry = self.state()["units"]["src/sample.cpp"]
        self.assertEqual(entry["status"], "pending", "Missing active brief searched archive")
        self.assertEqual(entry["previous_brief"], "docs/review_briefs/sample.md")

    def test_archive_reader_invalidates_changed_or_missing_brief_and_source(self):
        _, manifest = self.relocation_manifest()
        self.call(LEDGER.cmd_relocate_briefs, self.write_manifest(manifest))
        record = self.state_path.read_bytes()
        archived = self.root / manifest["entries"][0]["archived_brief"]
        original = archived.read_bytes()
        for change in ("brief", "missing", "source"):
            with self.subTest(change=change):
                self.state_path.write_bytes(record)
                archived.write_bytes(original)
                if change == "brief":
                    archived.write_bytes(b"Changed evidence\n")
                elif change == "missing":
                    archived.unlink()
                else:
                    self.source.write_bytes(b"int changed = 2;\n")
                self.call(LEDGER.cmd_inventory, quiet=True)
                self.assertEqual(self.state()["units"]["src/sample.cpp"]["status"], "pending")

    def test_archive_mark_remains_active_only(self):
        _, manifest = self.relocation_manifest()
        before = self.state_path.read_bytes()
        archived = manifest["entries"][0]["archived_brief"]
        with self.assertRaises(SystemExit):
            self.call(LEDGER.cmd_mark, "src/sample.cpp", archived,
                      LEDGER.working_tree_blob("src/sample.cpp"))
        self.assertEqual(self.state_path.read_bytes(), before)

    def test_archive_relocation_refuses_malformed_schema_and_duplicate_entries(self):
        _, manifest = self.relocation_manifest()
        entry = manifest["entries"][0]
        duplicate = {**entry, "archived_brief": "docs/archive/review_briefs/duplicate.md"}
        (self.root / duplicate["archived_brief"]).write_bytes(
            (self.root / entry["archived_brief"]).read_bytes())
        candidates = [{**manifest, "schema_version": True}, {**manifest, "extra": 1},
                      {**manifest, "entries": []}, {**manifest, "entries": [entry, entry]},
                      {**manifest, "entries": [entry, duplicate]},
                      {**manifest, "entries": [{**entry, "unexpected": 1}]},
                      {**manifest, "entries": [{**entry, "brief_sha256": False}]},
                      {**manifest, "entries": [{**entry, "brief_sha256": "0" * 63}]},
                      {**manifest, "entries": [{**entry, "path": "src/unknown.cpp"}]}]
        for candidate in candidates:
            with self.subTest(candidate=candidate):
                self.assert_relocation_refuses(candidate)
        with mock.patch.object(LEDGER, "MAX_RELOCATION_ENTRIES", 0):
            self.assert_relocation_refuses(manifest)

    def test_archive_relocation_refuses_duplicate_json_keys_and_oversized_manifest(self):
        _, manifest = self.relocation_manifest()
        path = Path(self.write_manifest(manifest))
        payload = path.read_bytes()
        state = self.state_path.read_bytes()
        duplicate_entry = payload.replace(b'"path": "src/sample.cpp"',
                                          b'"path": "src/wrong.cpp", "path": "src/sample.cpp"')
        self.assertNotEqual(duplicate_entry, payload)
        for bad in (b'{"schema_version":0,' + payload[1:], duplicate_entry,
                    payload + b" " * LEDGER.MAX_MIGRATION_MANIFEST_BYTES):
            path.write_bytes(bad)
            with self.assertRaises(SystemExit):
                self.call(LEDGER.cmd_relocate_briefs, str(path))
            self.assertEqual(self.state_path.read_bytes(), state)
            self.assertEqual(path.read_bytes(), bad)

    def test_archive_relocation_refuses_stale_state_and_unbound_credit(self):
        _, manifest = self.relocation_manifest()
        self.assert_relocation_refuses({**manifest, "state_sha256": "0" * 64})
        state = self.state()
        for change in ({"status": "pending"}, {"evidence": "cited"},
                       {"reviewed_brief_blob": None}):
            candidate = json.loads(json.dumps(state))
            candidate["units"]["src/sample.cpp"].update(change)
            self.state_path.write_text(json.dumps(candidate), encoding="utf-8")
            bound = {**manifest,
                     "state_sha256": hashlib.sha256(self.state_path.read_bytes()).hexdigest()}
            with self.subTest(change=change):
                self.assert_relocation_refuses(bound)
        state["schema_version"] = 1
        self.state_path.write_text(json.dumps(state), encoding="utf-8")
        self.assert_relocation_refuses({
            **manifest, "state_sha256": hashlib.sha256(self.state_path.read_bytes()).hexdigest()})

    def test_archive_relocation_refuses_changed_source_and_relabelled_review(self):
        _, manifest = self.relocation_manifest()
        self.source.write_bytes(b"int requires_new_review = 2;\n")
        self.assert_relocation_refuses(manifest)
        manifest["entries"][0]["reviewed_blob"] = LEDGER.working_tree_blob("src/sample.cpp")
        self.assert_relocation_refuses(manifest)

    def test_archive_relocation_refuses_raw_changes_hidden_by_git_filter(self):
        _, manifest = self.relocation_manifest()
        entry = manifest["entries"][0]
        archived = self.root / entry["archived_brief"]
        archived.write_bytes(archived.read_bytes().replace(b"\r\n", b"\n"))
        self.assertEqual(LEDGER.working_tree_blob(entry["archived_brief"]),
                         entry["reviewed_brief_blob"])
        self.assert_relocation_refuses(manifest)

    def test_archive_relocation_refuses_changed_old_filtered_binding(self):
        _, manifest = self.relocation_manifest()
        entry = manifest["entries"][0]
        archived = self.root / entry["archived_brief"]
        archived.write_bytes(b"Different evidence even with new raw confirmation\r\n")
        entry["brief_sha256"] = hashlib.sha256(archived.read_bytes()).hexdigest()
        self.assert_relocation_refuses(manifest)

    def test_archive_relocation_refuses_old_presence_and_invalid_new_paths(self):
        _, manifest = self.relocation_manifest()
        entry = manifest["entries"][0]
        archived = self.root / entry["archived_brief"]
        self.brief.write_bytes(archived.read_bytes())
        self.assert_relocation_refuses(manifest)
        self.brief.unlink()
        paths = ("docs/review_briefs/other.md", "docs/archive/sample.md",
                 "docs/archive/review_briefs/../review_briefs/sample.md",
                 "docs/archive/review_briefs/sample.md:stream",
                 "docs/archive/review_briefs/missing.md", "docs/archive/review_briefs/bad.cpp")
        for relative in (paths[0], paths[1], paths[-1]):
            (self.root / relative).write_bytes(archived.read_bytes())
        for path in paths:
            candidate = {**manifest, "entries": [{**entry, "archived_brief": path}]}
            with self.subTest(path=path):
                self.assert_relocation_refuses(candidate)

    def test_archive_relocation_requires_exact_old_active_path(self):
        _, manifest = self.relocation_manifest()
        entry = manifest["entries"][0]
        original = entry["brief"]
        for old_path in ("docs/review_briefs/other.md", "docs/archive/review_briefs/old.md"):
            with self.subTest(old_path=old_path):
                entry["brief"] = old_path
                if old_path.startswith("docs/archive/"):
                    state = self.state()
                    state["units"][entry["path"]]["brief"] = old_path
                    self.state_path.write_text(json.dumps(state), encoding="utf-8")
                    manifest["state_sha256"] = hashlib.sha256(
                        self.state_path.read_bytes()).hexdigest()
                self.assert_relocation_refuses(manifest)
        self.assertFalse((self.root / original).exists())

    def test_archive_relocation_checks_all_entries_and_duplicate_destinations(self):
        _, manifest = self.relocation_manifest(second_unit=True)
        second = manifest["entries"][-1]
        duplicate = {**manifest, "entries": [manifest["entries"][0],
                     {**second, "archived_brief": manifest["entries"][0]["archived_brief"]}]}
        self.assert_relocation_refuses(duplicate)
        second["brief_sha256"] = "0" * 64
        self.assert_relocation_refuses(manifest)

    def test_archive_relocation_refuses_junction_escape(self):
        _, manifest = self.relocation_manifest()
        # Inside the repository but outside the admitted brief root: repository containment
        # alone cannot make this junction safe.
        outside = self.root / "outside-archive"
        outside.mkdir()
        entry = manifest["entries"][0]
        evidence = outside / "sample.md"
        evidence.write_bytes((self.root / entry["archived_brief"]).read_bytes())
        link = self.root / "docs/archive/review_briefs/escape"
        subprocess.run(["cmd.exe", "/d", "/c", "mklink", "/J", str(link), str(outside)],
                       capture_output=True, check=True, creationflags=subprocess.CREATE_NO_WINDOW)
        self.addCleanup(link.rmdir)
        entry["archived_brief"] = "docs/archive/review_briefs/escape/sample.md"
        original = evidence.read_bytes()
        self.assert_relocation_refuses(manifest)
        self.assertEqual(evidence.read_bytes(), original)

    def test_archive_relocation_refuses_state_junction_escape(self):
        _, manifest = self.relocation_manifest()
        outside = self.fixture_root / "outside-state"
        scripts = self.root / "scripts"
        scripts.rename(outside)
        subprocess.run(["cmd.exe", "/d", "/c", "mklink", "/J", str(scripts), str(outside)],
                       capture_output=True, check=True, creationflags=subprocess.CREATE_NO_WINDOW)
        self.addCleanup(scripts.rmdir)
        self.assert_relocation_refuses(manifest)

    def test_archive_relocation_rechecks_complete_set_after_later_admission(self):
        _, manifest = self.relocation_manifest(second_unit=True)
        original_confirm = LEDGER.confirm_relocation_entry
        first = self.root / manifest["entries"][0]["archived_brief"]
        calls = 0
        def change_earlier_after_later(*arguments):
            nonlocal calls
            digest = original_confirm(*arguments)
            calls += 1
            if calls == 2:
                first.write_bytes(b"Late earlier-entry edit\n")
            return digest
        state = self.state_path.read_bytes()
        with mock.patch.object(LEDGER, "confirm_relocation_entry",
                               side_effect=change_earlier_after_later):
            with self.assertRaises(SystemExit):
                self.call(LEDGER.cmd_relocate_briefs, self.write_manifest(manifest))
        self.assertEqual(self.state_path.read_bytes(), state)
        self.assertEqual(first.read_bytes(), b"Late earlier-entry edit\n")

    def test_archive_relocation_rechecks_source_state_and_filter_before_write(self):
        _, manifest = self.relocation_manifest()
        original_confirm = LEDGER.confirm_relocation_entry
        original_state = self.state_path.read_bytes()
        original_source = self.source.read_bytes()
        attributes = self.root / ".gitattributes"
        original_attributes = attributes.read_bytes()
        for target, content in ((self.source, b"int late_source = 1;\n"),
                                (self.state_path, original_state + b"\n"),
                                (attributes, original_attributes + b"docs/archive/** -text\n")):
            self.state_path.write_bytes(original_state)
            self.source.write_bytes(original_source)
            attributes.write_bytes(original_attributes)
            calls = 0
            def change_after_admission(*arguments):
                nonlocal calls
                digest = original_confirm(*arguments)
                calls += 1
                if calls == 1:
                    target.write_bytes(content)
                return digest
            with self.subTest(target=target.name), mock.patch.object(
                    LEDGER, "confirm_relocation_entry", side_effect=change_after_admission):
                with self.assertRaises(SystemExit):
                    self.call(LEDGER.cmd_relocate_briefs, self.write_manifest(manifest))
                wanted = content if target == self.state_path else original_state
                self.assertEqual(self.state_path.read_bytes(), wanted)
                self.assertEqual(target.read_bytes(), content)

    def test_archive_relocation_atomic_failure_leaves_state_and_files_unchanged(self):
        _, manifest = self.relocation_manifest()
        with mock.patch.object(LEDGER.os, "replace", side_effect=OSError("fixture replace failure")):
            self.assert_relocation_refuses(manifest)
        self.assertEqual(list(self.state_path.parent.glob(".review-ledger-*")), [])

    def test_archive_relocation_real_cli_propagates_success_and_stale_refusal(self):
        _, manifest = self.relocation_manifest()
        script = self.root / "scripts/review_ledger.py"
        script.write_bytes(Path(LEDGER.__file__).read_bytes())
        manifest_path = self.write_manifest(manifest)
        command = [sys.executable, str(script), "relocate-briefs", "--manifest", manifest_path]
        result = subprocess.run(command, cwd=self.root, capture_output=True,
                                text=True, check=False, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("relocated 1 exact brief bindings; no new review credit", result.stdout)
        self.assertEqual(self.state()["units"]["src/sample.cpp"]["brief"],
                         "docs/archive/review_briefs/sample.md")
        before = self.state_path.read_bytes()
        result = subprocess.run(command, cwd=self.root, capture_output=True,
                                text=True, check=False, timeout=30)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("explicitly reviewed relocation state", result.stdout)
        self.assertEqual(self.state_path.read_bytes(), before)

    def test_archive_relocation_preserves_history_across_rereview_cycles(self):
        _, first = self.relocation_manifest()
        self.call(LEDGER.cmd_relocate_briefs, self.write_manifest(first))
        history = self.state()["brief_relocations"]
        archived = self.root / first["entries"][0]["archived_brief"]
        original = archived.read_bytes()
        self.brief.write_bytes(b"New explicit review after archival\r\n")
        _, second = self.relocation_manifest(destination_suffix="-second")
        self.assertEqual(self.state()["brief_relocations"], history)
        with mock.patch.object(LEDGER, "MAX_RELOCATION_HISTORY", 1):
            self.assert_relocation_refuses(second)
        second_path = self.write_manifest(second)
        self.call(LEDGER.cmd_relocate_briefs, second_path)
        expected = history + [{
            "original_state_sha256": second["state_sha256"],
            "manifest_sha256": hashlib.sha256(Path(second_path).read_bytes()).hexdigest(),
            "relocated_units": ["src/sample.cpp"],
        }]
        self.assertEqual(self.state()["brief_relocations"], expected,
                         "Relocation overwrote previous archival history")
        self.assertEqual(archived.read_bytes(), original)
        self.call(LEDGER.cmd_inventory, quiet=True)
        self.assertEqual(self.state()["brief_relocations"], expected)
        self.assertEqual(self.state()["units"]["src/sample.cpp"]["brief"],
                         "docs/archive/review_briefs/sample-second.md")
        self.assertEqual(self.state()["units"]["src/sample.cpp"]["status"], "reviewed")

    def test_archive_relocation_refuses_malformed_history(self):
        _, manifest = self.relocation_manifest()
        before = self.state()
        record = {"original_state_sha256": "1" * 64, "manifest_sha256": "2" * 64,
                  "relocated_units": ["src/earlier.cpp"]}
        malformed = [None, {}, [False], [{**record, "extra": True}],
                     [{**record, "manifest_sha256": "bad"}],
                     [{**record, "relocated_units": []}],
                     [{**record, "relocated_units": [False]}],
                     [{**record, "relocated_units": ["src/same.cpp", "src/same.cpp"]}]]
        for history in malformed:
            with self.subTest(history=history):
                state = {**before, "brief_relocations": history}
                self.state_path.write_text(json.dumps(state), encoding="utf-8")
                candidate = {**manifest, "state_sha256": hashlib.sha256(
                    self.state_path.read_bytes()).hexdigest()}
                self.assert_relocation_refuses(candidate)

    def test_archive_relocation_validates_pinned_snapshot_not_intervening_state(self):
        prior, manifest = self.relocation_manifest(second_unit=True)
        unselected = manifest["entries"].pop()
        (self.root / unselected["archived_brief"]).rename(self.root / unselected["brief"])
        pinned = self.state_path.read_bytes()
        alternate = json.loads(pinned)
        alternate["retained_history"]["checkpoint"] = "unapproved alternate state"
        alternate["units"][unselected["path"]]["previous_evidence"] = "unapproved metadata"
        original_load = LEDGER.load_state
        def swap_only_during_validation(*arguments, **keywords):
            self.state_path.write_text(json.dumps(alternate), encoding="utf-8")
            try:
                return original_load(*arguments, **keywords)
            finally:
                self.state_path.write_bytes(pinned)
        with mock.patch.object(LEDGER, "load_state", side_effect=swap_only_during_validation):
            self.assertEqual(self.call(LEDGER.cmd_relocate_briefs, self.write_manifest(manifest)), 0)
        updated = self.state()
        self.assertEqual(updated["retained_history"], prior["retained_history"],
                         "Relocation published history from unpinned state bytes")
        self.assertEqual(updated["units"][unselected["path"]], prior["units"][unselected["path"]],
                         "Relocation published unselected metadata from unpinned state bytes")


if __name__ == "__main__":
    unittest.main()
