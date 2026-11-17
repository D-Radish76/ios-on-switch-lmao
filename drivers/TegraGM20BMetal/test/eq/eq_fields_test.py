"""eq_fields.py's frame on a log made by hand: a strip raw drew a triangle at a time folds back into one draw."""

import tempfile
import unittest
from pathlib import Path

import eq_fields


def draw(first: int, barriers: str, front: str, extra: str = '') -> str:
    return (
        f'DRAW first={first} count=3 begin=00000000 barriers={barriers}\n'
        f'  regs={front}{extra}\n'
        f'  R 300=3 191c={front}{extra}\n'
        '  rt0=R1 64x64 fmt=cf mem=1000 pitch=0\n'
        f'  vb0 stride=48 data={first}:90\n'
        '  stage5 prog=5 regs=24 group=4\n'
        f'    rootwords draw: {first} 0 0 push: 0\n'
    )


def frame(text: str) -> eq_fields.Frame:
    with tempfile.TemporaryDirectory() as tmp:
        log = Path(tmp) / 'raw.log'
        log.write_text(text)
        return eq_fields.frame(str(log))


class Runs(unittest.TestCase):
    def test_a_strips_runs_fold_across_submissions_whatever_each_sets_of_its_own(self):
        f = frame(
            'SUBMIT 1\n'
            + draw(0, 'PSB,TEXINV', '900')
            + 'SUBMIT 2\n'
            + draw(1, 'PSB,SPLIT,TEXINV', '901')
            + 'SUBMIT 3\n'
        )
        self.assertEqual(len(f.draws), 1)
        self.assertEqual(f.draws[0].fields['draw'].split()[1:3], ['0', '4'])
        self.assertEqual(f.draws[0].registers['191c'], '900')

    def test_a_run_of_other_state_stays_a_draw(self):
        f = frame(
            'SUBMIT 1\n' + draw(0, 'PSB,TEXINV', '900') + draw(1, 'PSB,SPLIT,TEXINV', '901', ' 1a00=1') + 'SUBMIT 2\n'
        )
        self.assertEqual(len(f.draws), 2)

    def test_a_draw_that_is_no_run_stays_a_draw(self):
        f = frame('SUBMIT 1\n' + draw(0, 'PSB,TEXINV', '900') + draw(1, 'PSB,TEXINV', '901') + 'SUBMIT 2\n')
        self.assertEqual(len(f.draws), 2)


class IteratedState(unittest.TestCase):
    def fields(self, registers: str) -> dict[str, str]:
        return eq_fields.digest_fields(frame('SUBMIT 1\n' + draw(0, '-', '900', registers) + 'SUBMIT 2\n').draws[0])

    def test_a_disabled_iterated_program_leaves_the_draws_live_state_unchanged(self):
        mode = ' dd0=0 12e4=1'
        residue = ' dd4=9 df0=0 df4=c811 1480=34cd 1340=8006 1344=4001 1358=4303'
        self.assertEqual(self.fields(mode), self.fields(mode + residue))

    def test_active_programs_and_common_blend_equations_still_change_the_draw(self):
        for mode, changed in (
            (' dd0=1 12e4=0', ' dd4=9 df4=c811'),
            (' dd0=1 12e4=0', ' 1480=34cd'),
            (' dd0=1 12e4=1', ' 1480=34cd'),
            (' dd0=2 12e4=1', ' 1480=34cd'),
            (' dd0=3 12e4=1', ' df4=c811'),
            (' dd0=0 12e4=0', ' 1340=8006 1344=4001 1358=4303'),
            (' 12e4=1', ' dd4=9 df4=c811'),
            (' dd0=0', ' 1340=8006'),
        ):
            with self.subTest(mode=mode, changed=changed):
                self.assertNotEqual(self.fields(mode), self.fields(mode + changed))

    def test_global_colour_key_remains_live_when_iterated_blend_is_disabled(self):
        mode = ' dd0=0 12e4=1'
        self.assertNotEqual(self.fields(mode), self.fields(mode + ' 1354=12345678'))


if __name__ == '__main__':
    unittest.main()
