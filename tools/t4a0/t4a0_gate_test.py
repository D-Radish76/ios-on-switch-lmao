"""T4-A0: the gate state machine against a modelled watchdog block, host only."""

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

HERE = Path('tools/t4a0')
SOURCES = ('gate.c', 'sim.c', 'sequence.c')
TARGET = 't4a0_gate_test'
SOURCE_SUFFIXES = ('.c', '.h', '.py', '.bazel')
# What rules_python writes next to a py_test inside its runfiles tree: the launcher, its build data and its manifest.
BAZEL_GENERATED = (TARGET, f'{TARGET}.build_data.txt', f'{TARGET}.exe', f'{TARGET}.runfiles_manifest', f'{TARGET}.runfiles')
DESIGN = Path('ERISTA_T4A_DESIGN.md').read_text()


class Host(unittest.TestCase):
    @unittest.skipUnless(shutil.which('clang') or shutil.which('cc'), 'no C compiler on PATH')
    def test_the_c_tests_pass_with_warnings_as_errors(self) -> None:
        compiler = shutil.which('clang') or shutil.which('cc')
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / 'gate_test'
            built = subprocess.run(
                [compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', f'-I{HERE}',
                 *(str(HERE / s) for s in (*SOURCES, 'gate_test.c')), '-o', str(out)],
                capture_output=True, text=True, check=False,
            )  # fmt: skip
            self.assertEqual(0, built.returncode, built.stderr)
            ran = subprocess.run([str(out)], capture_output=True, text=True, check=False)
        self.assertEqual(0, ran.returncode, ran.stdout[-2000:])
        self.assertRegex(ran.stdout, r't4a0 gate: \d+ checks, 0 failures')
        self.assertIn('fault pairs:', ran.stdout)


def in_bazel() -> bool:
    return bool(os.environ.get('TEST_SRCDIR') and os.environ.get('TEST_TARGET'))


def unexpected(directory: Path, bazel: bool) -> list[str]:
    """Names in the directory that are neither a source nor, inside a Bazel test run only, a file Bazel writes for this test."""
    return sorted(
        p.name
        for p in directory.iterdir()
        if p.name != '__pycache__'
        and p.suffix not in SOURCE_SUFFIXES
        and not (bazel and p.name in BAZEL_GENERATED)
    )  # fmt: skip


class Isolation(unittest.TestCase):
    def test_the_check_tells_a_runfiles_tree_from_a_source_tree(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            for name in ('gate.c', 'gate.h', 'BUILD.bazel', 'a.py', *BAZEL_GENERATED):
                (d / name).write_text('')
            self.assertEqual([], unexpected(d, bazel=True))
            self.assertEqual(sorted(BAZEL_GENERATED), unexpected(d, bazel=False))
            for stray in ('x.bin', 'other.txt', 'loader', f'{TARGET}2', 'gate.o', 'gate.cc'):
                (d / stray).write_text('')
                self.assertEqual([stray], [n for n in unexpected(d, bazel=True) if n == stray], stray)
                (d / stray).unlink()

    def test_the_real_directory_has_only_sources_and_what_bazel_writes(self) -> None:
        self.assertEqual([], unexpected(HERE, bazel=in_bazel()))

    def test_nothing_reaches_a_device_or_the_loader(self) -> None:
        for name in (*SOURCES, 'gate.h', 'sim.h', 'sequence.h', 'gate_test.c'):
            text = (HERE / name).read_text()
            self.assertNotRegex(text, r'\bvolatile\b|\basm\b|__asm__|\bsmc\b', name)
            for include in re.findall(r'#include <([^>]+)>', text):
                self.assertIn(include, ('stdint.h', 'stdio.h', 'stdlib.h', 'string.h'), name)
            self.assertNotIn('boot/', text, name)
            self.assertLess(len(text.splitlines()), 500, name)

    def test_the_gate_has_one_store(self) -> None:
        self.assertEqual(1, len(re.findall(r'io->write\(', (HERE / 'gate.c').read_text())))
        self.assertNotIn('->write(', (HERE / 'sequence.c').read_text())

    def test_no_build_target_links_it_and_no_loader_includes_it(self) -> None:
        build = (HERE / 'BUILD.bazel').read_text()
        self.assertNotRegex(build, r'\bcc_(binary|library|test)\b|genrule|diag_images')
        for path in Path('boot').rglob('*'):
            if path.suffix in ('.c', '.h', '.S', '.bazel', '.bzl'):
                self.assertNotIn('t4a0', path.read_text(errors='ignore'), str(path))


class Design(unittest.TestCase):
    def test_the_design_states_the_a0_a1_a2_split_and_the_clarified_rules(self) -> None:
        for phrase in ('T4-A0', 'T4-A1', 'T4-A2', 'STOP_FAILED', 'case surface temperature', 'does not show that the CPU stopped'):
            self.assertIn(phrase, DESIGN)


if __name__ == '__main__':
    unittest.main()
