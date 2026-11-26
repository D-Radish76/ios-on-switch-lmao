"""dump.sh reads exactly the registers native_gate compares, and only reads."""

import os
import re
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path('tools/mc_carveout/dump.sh')
LOADER = Path('boot/loader/carveout.h').read_text()


def gate_table():
    body = LOADER[LOADER.index('switch_carveout_oled[][2] = {') :]
    body = body[: body.index('};')]
    return [(int(a, 16), int(v, 0)) for a, v in re.findall(r'\{(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+|\d+)\}', body)]


def run(devmem_body, applet=True, standalone=False):
    """dump.sh with a fake busybox whose devmem applet runs `devmem_body`; a standalone devmem that must never run."""
    with tempfile.TemporaryDirectory() as tmp:
        fake = Path(tmp) / 'busybox'
        fake.write_text(
            '#!/bin/sh\nif [ "$1" = --list ]; then ' + ('echo devmem; ' if applet else '') + 'exit 0; fi\n'
            '[ "$1" = devmem ] || exit 9\nshift\n' + devmem_body
        )
        fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
        if standalone:
            loose = Path(tmp) / 'devmem'
            loose.write_text('#!/bin/sh\necho 0xdead\n')
            loose.chmod(loose.stat().st_mode | stat.S_IXUSR)
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

    def test_a_standalone_devmem_is_never_used(self):
        out = run('echo 0x1\n', standalone=True)
        self.assertNotIn('dead', out.stdout)
        self.assertIn('DIFFERENT', out.stdout)

    def test_without_the_busybox_applet_it_stops(self):
        out = run('echo 0\n', applet=False, standalone=True)
        self.assertEqual((out.returncode, out.stdout), (2, ''))

    def test_an_unreadable_register_stops_it(self):
        self.assertEqual(run('exit 1\n').returncode, 3)

    def test_the_reader_is_only_called_to_read(self):
        text = SCRIPT.read_text()
        code = '\n'.join(line for line in text.splitlines() if not line.lstrip().startswith('#'))
        calls = re.findall(r'busybox devmem[^\n]*', code)
        self.assertEqual(calls, ['busybox devmem "$address" 32) || { echo "$address unreadable" >&2; exit 3; }'])
        self.assertNotRegex(code, r'/dev/mem|\bdd\b|devmem2|\$reader')


if __name__ == '__main__':
    unittest.main()
