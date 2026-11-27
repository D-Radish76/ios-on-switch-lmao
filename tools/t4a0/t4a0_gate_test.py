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
# Entries Bazel puts next to the test in its runfiles tree. MEASURED were listed on the Mac (2f3f8314b, 2026-11-27);
# ESTIMATED are what other platforms or rules_python versions may write and have not been seen. 'entry' is a file or a
# link to one, 'dir' is a directory.
MEASURED = {
    f'_{TARGET}_stage2_bootstrap.py': 'entry',
    f'_{TARGET}.venv': 'dir',
    TARGET: 'entry',
    f'{TARGET}.build_data.txt': 'entry',
}
ESTIMATED = {f'{TARGET}.exe': 'entry', f'{TARGET}.runfiles_manifest': 'entry', f'{TARGET}.runfiles': 'dir'}
BAZEL_GENERATED = {**MEASURED, **ESTIMATED}
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


def generated_as_bazel_writes_it(path: Path) -> bool:
    kind = BAZEL_GENERATED.get(path.name)
    return kind == 'dir' and path.is_dir() and not path.is_symlink() or kind == 'entry' and not path.is_dir()


def unexpected(directory: Path, bazel: bool) -> list[str]:
    """Names that are neither a source nor, inside a Bazel test run only, an entry Bazel writes for this test by its name and kind."""
    return sorted(
        p.name
        for p in directory.iterdir()
        if p.name != '__pycache__'
        and not (bazel and generated_as_bazel_writes_it(p))
        and (p.suffix not in SOURCE_SUFFIXES or p.name in BAZEL_GENERATED)
    )  # fmt: skip


class Isolation(unittest.TestCase):
    def make_tree(self, directory: Path) -> None:
        for name in ('BUILD.bazel', 'gate_test.c', 'gate.c', 'gate.h', 'sequence.c', 'sequence.h', 'sim.c', 'sim.h', 't4a0_gate_test.py'):
            (directory / name).write_text('')
        target = directory / 'real_target'
        target.write_text('')
        for name, kind in MEASURED.items():
            if kind == 'dir':
                (directory / name).mkdir()
            else:
                (directory / name).symlink_to(target)
        target.unlink()
        (directory / 'real_target').write_text('')

    def test_the_measured_runfiles_tree_passes_only_inside_bazel(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            self.make_tree(d)
            self.assertEqual(['real_target'], unexpected(d, bazel=True))
            (d / 'real_target').unlink()
            self.assertEqual([], unexpected(d, bazel=True))
            self.assertEqual(sorted(MEASURED), unexpected(d, bazel=False))

    def test_estimated_entries_are_allowed_in_bazel_and_refused_in_a_source_tree(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            for name, kind in ESTIMATED.items():
                (d / name).mkdir() if kind == 'dir' else (d / name).write_text('')
            self.assertEqual([], unexpected(d, bazel=True))
            self.assertEqual(sorted(ESTIMATED), unexpected(d, bazel=False))

    def test_a_generated_name_of_the_wrong_kind_and_unknown_entries_are_refused_in_bazel(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / f'_{TARGET}.venv').write_text('')
            (d / TARGET).mkdir()
            (d / '_other.venv').mkdir()
            (d / f'_{TARGET}2.venv').mkdir()
            (d / 'x.bin').write_text('')
            (d / 'gate.o').write_text('')
            (d / 'subdir').mkdir()
            expected = [f'_{TARGET}.venv', TARGET, '_other.venv', f'_{TARGET}2.venv', 'x.bin', 'gate.o', 'subdir']
            self.assertEqual(sorted(expected), unexpected(d, bazel=True))

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
