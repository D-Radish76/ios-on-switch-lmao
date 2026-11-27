"""T4-A0: the gate state machine against a modelled watchdog block, host only."""

import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

HERE = Path('tools/t4a0')
SOURCES = ('gate.c', 'sim.c', 'sequence.c')
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


class Isolation(unittest.TestCase):
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

    def test_only_the_allowed_source_extensions_exist(self) -> None:
        for path in (p for p in HERE.iterdir() if p.is_file()):
            self.assertIn(path.suffix, ('.c', '.h', '.py', '.bazel'), str(path))


class Design(unittest.TestCase):
    def test_the_design_states_the_a0_a1_a2_split_and_the_clarified_rules(self) -> None:
        for phrase in ('T4-A0', 'T4-A1', 'T4-A2', 'STOP_FAILED', 'case surface temperature', 'does not show that the CPU stopped'):
            self.assertIn(phrase, DESIGN)


if __name__ == '__main__':
    unittest.main()
