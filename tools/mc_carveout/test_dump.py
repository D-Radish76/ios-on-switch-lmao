"""dump.sh reads exactly the registers native_gate compares, and only reads."""

import os
import re
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path('tools/mc_carveout/dump.sh')
LOADER = Path('boot/loader/loader.c').read_text()


def gate_table():
    body = LOADER[LOADER.index('static const uint32_t mc[][2] = {') :]
    body = body[: body.index('};')]
    return [(int(a, 16), int(v, 0)) for a, v in re.findall(r'\{(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+|\d+)\}', body)]


def run(devmem_body):
    with tempfile.TemporaryDirectory() as tmp:
        fake = Path(tmp) / 'devmem'
        fake.write_text('#!/bin/sh\n' + devmem_body)
        fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
        env = {**os.environ, 'PATH': f'{tmp}:/usr/bin:/bin'}
        return subprocess.run(['/bin/sh', str(SCRIPT)], capture_output=True, text=True, env=env, check=False)


class DumpTest(unittest.TestCase):
    def test_it_lists_the_registers_native_gate_compares(self):
        rows = [line.split() for line in run('echo 0\n').stdout.splitlines()]
        self.assertEqual([(int(r[0], 16), int(r[3], 16)) for r in rows], gate_table())
        self.assertEqual(len(rows), 25)

    def test_the_oled_values_read_back_as_same(self):
        values = '\n'.join(f'  {a:#010x}) echo {v:#x};;' for a, v in gate_table())
        out = run(f'case "$1" in\n{values}\nesac\n')
        self.assertEqual(out.returncode, 0)
        self.assertNotIn('DIFFERENT', out.stdout)

    def test_a_different_value_is_marked(self):
        out = run('echo 0x1\n')
        self.assertEqual(out.returncode, 0)
        self.assertIn('DIFFERENT', out.stdout)

    def test_an_unreadable_register_stops_it(self):
        self.assertEqual(run('exit 1\n').returncode, 3)

    def test_the_reader_is_only_called_to_read(self):
        text = SCRIPT.read_text()
        self.assertEqual(re.findall(r'\$reader [^\n]*', text), ['$reader "$address" 32) || { echo "$address unreadable" >&2; exit 3; }'])
        self.assertNotRegex(text, r'/dev/mem|\bdd\b|devmem2')


if __name__ == '__main__':
    unittest.main()
