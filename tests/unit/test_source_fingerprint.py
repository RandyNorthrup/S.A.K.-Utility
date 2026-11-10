"""Real CMake source-stamp generation and rebuild invalidation contracts."""

import hashlib
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
MODULE = PROJECT_ROOT / "cmake" / "SakSourceFingerprint.cmake"
VERIFIER = PROJECT_ROOT / "scripts" / "verify_source_fingerprint.py"


class SourceFingerprintFixture(unittest.TestCase):
    def setUp(self):
        self.cmake = shutil.which("cmake")
        self.assertIsNotNone(self.cmake, "CMake is required for source stamp tests")
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "source"
        self.source.mkdir()
        self.build = self.root / "build"
        self.source.joinpath("a.cpp").write_bytes(b"source one\n")
        self.source.joinpath("b.h").write_bytes(b"header one\n")

    def configure(self, inputs=("a.cpp", "b.h"), module=MODULE):
        files = " ".join(f'"{item}"' for item in inputs)
        self.source.joinpath("CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.24)\n"
            "project(StampFixture NONE)\n"
            f'include("{module.as_posix()}")\n'
            f'sak_compute_source_fingerprint(STAMP "${{CMAKE_CURRENT_SOURCE_DIR}}" {files})\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/stamp.txt" "${STAMP}")\n'
            'file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/inputs.txt" '
            'CONTENT "${STAMP_RECORDS}" NEWLINE_STYLE LF)\n',
            encoding="utf-8",
        )
        return subprocess.run(
            [self.cmake, "-S", str(self.source), "-B", str(self.build),
             "-G", "Visual Studio 17 2022", "-A", "x64"],
            capture_output=True, text=True, check=False, timeout=30,
        )

    def stamp(self):
        return self.build.joinpath("stamp.txt").read_text(encoding="ascii")

    def assert_configured(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def make_junction(self, link, destination):
        subprocess.run(["cmd.exe", "/d", "/c", "mklink", "/J", str(link), str(destination)],
                       capture_output=True, check=True, creationflags=subprocess.CREATE_NO_WINDOW)
        self.addCleanup(self.remove_junction, link)

    def remove_junction(self, link):
        try:
            info = link.lstat()
        except FileNotFoundError:
            return  # A prior fixture cleanup already removed this exact link.
        if info.st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT:
            self.assertTrue(link.resolve().is_relative_to(self.root.resolve()))
            link.rmdir()


class SourceFingerprintTests(SourceFingerprintFixture):
    def test_exact_path_and_content_hashes_are_bound(self):
        self.assert_configured(self.configure())
        records = "".join(
            f"{name}:{hashlib.sha256(self.source.joinpath(name).read_bytes()).hexdigest()}\n"
            for name in ("a.cpp", "b.h")
        )
        self.assertEqual(self.stamp(), hashlib.sha256(records.encode()).hexdigest())
        self.assertEqual(self.build.joinpath("inputs.txt").read_bytes(), records.encode("utf-8"))

    def test_order_and_duplicate_inputs_do_not_change_stamp(self):
        self.assert_configured(self.configure())
        original = self.stamp()
        self.assert_configured(self.configure(("b.h", "a.cpp", "b.h")))
        self.assertEqual(self.stamp(), original)

    def test_header_change_reconfigures_on_build(self):
        self.assert_configured(self.configure())
        original = self.stamp()
        self.source.joinpath("b.h").write_bytes(b"header changed\n")
        result = subprocess.run(
            [self.cmake, "--build", str(self.build), "--config", "Release"],
            capture_output=True, text=True, check=False, timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotEqual(self.stamp(), original)

    def test_rename_changes_stamp_even_with_identical_bytes(self):
        self.assert_configured(self.configure())
        original = self.stamp()
        self.source.joinpath("b.h").rename(self.source / "c.h")
        self.assert_configured(self.configure(("a.cpp", "c.h")))
        self.assertNotEqual(self.stamp(), original)

    def test_missing_empty_and_outside_inputs_refuse(self):
        self.root.joinpath("outside.h").write_bytes(b"outside\n")
        for inputs, diagnostic in (
            (("missing.h",), "missing or not a file"),
            ((), "nonempty input list"),
            (("../outside.h",), "escapes source root"),
        ):
            with self.subTest(inputs=inputs):
                result = self.configure(inputs)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(diagnostic, result.stdout + result.stderr)

    def test_windows_junction_input_is_refused_during_configure(self):
        outside = self.root / "outside"
        outside.mkdir()
        (outside / "header.h").write_bytes(b"outside\n")
        self.make_junction(self.source / "headers", outside)
        result = self.configure(("a.cpp", "headers/header.h"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Reparse", result.stdout + result.stderr)

    def test_in_root_junction_input_is_also_refused(self):
        original = self.source / "ordinary"
        original.mkdir()
        (original / "header.h").write_bytes(b"inside\n")
        self.make_junction(self.source / "headers", original)
        result = self.configure(("a.cpp", "headers/header.h"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Reparse", result.stdout + result.stderr)

    def test_verifier_rejects_changed_manifest_bytes_and_noncanonical_records(self):
        self.assert_configured(self.configure())
        manifest = self.build / "inputs.txt"
        original = manifest.read_bytes()
        for payload, expected, diagnostic in (
            (original + b"extra\n", self.stamp(), "manifest bytes differ"),
            (original.replace(b"\n", b"\r\n"), None, "exact UTF-8 LF"),
            (original + original, None, "repeats an input"),
            (b"../outside.h:" + b"a" * 64 + b"\n", None, "invalid record"),
        ):
            with self.subTest(diagnostic=diagnostic):
                manifest.write_bytes(payload)
                digest = expected or hashlib.sha256(payload).hexdigest()
                result = subprocess.run(
                    [sys.executable, str(VERIFIER), "verify", "--source-root", str(self.source),
                     "--manifest", str(manifest), "--expected-sha256", digest],
                    capture_output=True, text=True, timeout=30)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(diagnostic, result.stdout + result.stderr)


class NativeSourceFingerprintTests(SourceFingerprintFixture):
    """Real C/C++ objects and an archive, without any shared project build or disk operation."""

    def write_native_fixture(self):
        (self.source / "cmake").mkdir()
        (self.source / "scripts").mkdir()
        (self.source / "headers").mkdir()
        shutil.copyfile(MODULE, self.source / "cmake/SakSourceFingerprint.cmake")
        shutil.copyfile(VERIFIER, self.source / "scripts/verify_source_fingerprint.py")
        (self.source / "headers/values.h").write_bytes(b"#define HEADER_VALUE 10\n")
        (self.source / "shim.c").write_bytes(b"int shim_value(void) { return 1; }\n")
        (self.source / "codec.cpp").write_bytes(
            b'#include "headers/values.h"\nextern "C" int shim_value(void);\n'
            b'int codec_value() { return HEADER_VALUE + shim_value() + 100; }\n')
        (self.source / "main.cpp").write_bytes(
            b'#include <cstdio>\nint codec_value();\n'
            b'constexpr char source_stamp[] = SAK_TEST_REPORT_STAMP;\n'
            b'int main() { std::printf("%d %s\\n", codec_value(), source_stamp); }\n')
        (self.source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.24)\nproject(NativeStampFixture C CXX)\n"
            'include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/SakSourceFingerprint.cmake")\n'
            'add_library(shim OBJECT shim.c)\n'
            'add_library(codec STATIC codec.cpp $<TARGET_OBJECTS:shim>)\n'
            'add_executable(app main.cpp)\ntarget_link_libraries(app PRIVATE codec)\n'
            'sak_compute_source_fingerprint(STAMP "${CMAKE_CURRENT_SOURCE_DIR}" '
            'main.cpp codec.cpp shim.c headers/values.h scripts/verify_source_fingerprint.py)\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/stamp.txt" "${STAMP}")\n'
            'file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/inputs.txt" CONTENT "${STAMP_RECORDS}" '
            'NEWLINE_STYLE LF)\n'
            'set_property(SOURCE main.cpp APPEND PROPERTY COMPILE_DEFINITIONS SAK_TEST_REPORT_STAMP="${STAMP}")\n'
            'sak_add_source_fingerprint_guard(app "${CMAKE_CURRENT_SOURCE_DIR}" '
            '"${CMAKE_BINARY_DIR}/inputs.txt" "${STAMP}" codec shim)\n'
            'add_custom_command(TARGET app POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different '
            '"${CMAKE_BINARY_DIR}/inputs.txt" "$<TARGET_FILE_DIR:app>/app.sources.txt" VERBATIM)\n',
            encoding="utf-8")

    def configure_native(self):
        return subprocess.run(
            [self.cmake, "-S", str(self.source), "-B", str(self.build),
             "-G", "Visual Studio 17 2022", "-A", "x64"],
            capture_output=True, text=True, timeout=90, creationflags=subprocess.CREATE_NO_WINDOW)

    def build_native(self):
        return subprocess.run(
            [self.cmake, "--build", str(self.build), "--config", "Release", "--target", "app"],
            capture_output=True, text=True, timeout=90, creationflags=subprocess.CREATE_NO_WINDOW)

    def native_value(self):
        result = subprocess.run([str(self.build / "Release/app.exe")], capture_output=True,
                                text=True, timeout=10, check=True,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        value, stamp = result.stdout.split()
        return int(value), stamp

    def assert_native_built(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_backdated_edits_reject_then_recompile_real_sources_headers_and_archive(self):
        self.write_native_fixture()
        self.assert_configured(self.configure_native())
        self.assert_native_built(self.build_native())
        previous_value = 111
        self.assertEqual(self.native_value(), (previous_value, self.stamp()))
        for relative, old, new, expected in (
            ("codec.cpp", b"+ 100", b"+ 200", 211),
            ("headers/values.h", b"VALUE 10", b"VALUE 20", 221),
            ("shim.c", b"return 1", b"return 2", 222),
        ):
            with self.subTest(input=relative):
                path = self.source / relative
                before = path.stat()
                prior_stamp = self.stamp()
                original = path.read_bytes()
                self.assertEqual(original.count(old), 1)
                path.write_bytes(original.replace(old, new))
                os.utime(path, ns=(before.st_atime_ns, before.st_mtime_ns))
                self.assertEqual(path.stat().st_size, before.st_size)
                self.assertEqual(path.stat().st_mtime_ns, before.st_mtime_ns)
                rejected = self.build_native()
                self.assertNotEqual(rejected.returncode, 0)
                self.assertIn("Source bytes changed since configuration", rejected.stdout + rejected.stderr)
                self.assertEqual(self.native_value(), (previous_value, prior_stamp))
                self.assert_configured(self.configure_native())
                self.assertNotEqual(self.stamp(), prior_stamp)
                self.assert_native_built(self.build_native())
                self.assertEqual(self.native_value(), (expected, self.stamp()))
                self.assertEqual((self.build / "Release/app.sources.txt").read_bytes(),
                                 (self.build / "inputs.txt").read_bytes())
                previous_value = expected

    def test_post_configure_junction_insertion_and_retargeting_refuse_build(self):
        self.write_native_fixture()
        self.assert_configured(self.configure_native())
        self.assert_native_built(self.build_native())
        original_stamp = self.stamp()
        headers = self.source / "headers"
        saved = self.source / "saved-headers"
        self.assertTrue(headers.resolve().is_relative_to(self.root.resolve()))
        self.assertTrue(saved.absolute().is_relative_to(self.root.resolve()))
        headers.rename(saved)
        self.make_junction(headers, saved)
        rejected = self.build_native()
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("Reparse", rejected.stdout + rejected.stderr)
        self.assertEqual(self.native_value(), (111, original_stamp))

        self.remove_junction(headers)
        outside = self.root / "outside-headers"
        outside.mkdir()
        (outside / "values.h").write_bytes((saved / "values.h").read_bytes())
        stamp = (saved / "values.h").stat()
        os.utime(outside / "values.h", ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        self.make_junction(headers, outside)
        rejected = self.build_native()
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("Reparse", rejected.stdout + rejected.stderr)
        self.assertEqual(self.native_value(), (111, original_stamp))

    def test_uppercase_header_reconfigure_rebuilds_real_archive(self):
        self.write_native_fixture()
        header = self.source / "headers/values.H"
        (self.source / "headers/values.h").rename(header)
        codec = self.source / "codec.cpp"
        codec.write_bytes(codec.read_bytes().replace(b"values.h", b"values.H"))
        cmake_file = self.source / "CMakeLists.txt"
        cmake_file.write_text(cmake_file.read_text().replace("headers/values.h", "headers/values.H"),
                              encoding="utf-8")
        self.assert_configured(self.configure_native())
        self.assert_native_built(self.build_native())
        self.assertEqual(self.native_value(), (111, self.stamp()))
        before = header.stat()
        header.write_bytes(header.read_bytes().replace(b"VALUE 10", b"VALUE 20"))
        os.utime(header, ns=(before.st_atime_ns, before.st_mtime_ns))
        self.assert_configured(self.configure_native())
        self.assert_native_built(self.build_native())
        self.assertEqual(self.native_value(), (121, self.stamp()))

    def test_uppercase_source_reconfigure_rebuilds_real_archive(self):
        self.write_native_fixture()
        codec = self.source / "codec.CPP"
        (self.source / "codec.cpp").rename(codec)
        cmake_file = self.source / "CMakeLists.txt"
        code = cmake_file.read_text().replace("codec.cpp", "codec.CPP")
        code = code.replace('add_library(codec STATIC',
                            'set_source_files_properties(codec.CPP PROPERTIES LANGUAGE CXX)\n'
                            'add_library(codec STATIC')
        cmake_file.write_text(code, encoding="utf-8")
        self.assert_configured(self.configure_native())
        self.assert_native_built(self.build_native())
        self.assertEqual(self.native_value(), (111, self.stamp()))
        before = codec.stat()
        codec.write_bytes(codec.read_bytes().replace(b"+ 100", b"+ 200"))
        os.utime(codec, ns=(before.st_atime_ns, before.st_mtime_ns))
        self.assert_configured(self.configure_native())
        self.assert_native_built(self.build_native())
        self.assertEqual(self.native_value(), (211, self.stamp()))


if __name__ == "__main__":
    unittest.main()
