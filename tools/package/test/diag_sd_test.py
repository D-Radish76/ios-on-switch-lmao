"""The SD diagnostic (D1-pre): its allowed commands, its images, and the exporter conditions on mmc info."""

import unittest
from pathlib import Path

from bl33 import bl33_patch
from diag_boot import DWELL, diag_bl33, diag_env_edits, diag_u_boot
from diag_memory import ini, memory_script
from diag_sd import PAGES
from script_commands import FORBIDDEN, commands
from sd_requirements import MET, NOT_MET, NOT_SHOWN, judge, mask

ROOT = Path('.')
HEADER = (ROOT / 'kits/SwitchBoard/SwitchBoard.h').read_text()
FRAME = (ROOT / 'boot/diag/sd/u-boot-script.txt').read_text()
BODIES = {
    1: (ROOT / 'boot/diag/sd/page-1-state.txt').read_text(),
    2: (ROOT / 'boot/diag/sd/page-2-info.txt').read_text(),
}
BL33 = (ROOT / 'bazel/pinned/firmware/bl33.bin').read_bytes()
COMMANDS = {'setenv', 'test', 'echo', 'mmc', 'part', 'sleep', 'reset', 'true'}
MMC_FORMS = {1: [['mmc', 'list'], ['part', 'list', 'mmc', '1']], 2: [['mmc', 'dev', '1'], ['mmc', 'info']]}
SYNTHETIC_INFO = """Device: sdhci@700b0000
Manufacturer ID: 3
OEM: 5344
Name: SL32G
Tran Speed: 50000000
Rd Block Len: 512
SD version 3.0
High Capacity: Yes
Capacity: 29.7 GiB
Bus Width: 4-bit
Erase Group Size: 512 Bytes
"""


def script(page: int) -> str:
    return memory_script(FRAME, BODIES[page], HEADER, page)


class Scripts(unittest.TestCase):
    def test_only_the_listed_commands_run(self) -> None:
        for page in (1, 2):
            for tokens in commands(script(page)):
                self.assertIn(tokens[0], COMMANDS, ' '.join(tokens))

    def test_the_mmc_and_part_forms_are_exactly_these(self) -> None:
        for page in (1, 2):
            used = [t for t in commands(script(page)) if t[0] in ('mmc', 'part')]
            self.assertEqual(used, MMC_FORMS[page])

    def test_mmc_info_never_runs_before_mmc_dev_selects_the_card(self) -> None:
        """Without `mmc dev` the current device is 0 (the internal storage), and `mmc info` would start it."""
        self.assertNotIn(['mmc', 'info'], commands(script(1)))
        two = [t for t in commands(script(2)) if t[0] == 'mmc']
        self.assertLess(two.index(['mmc', 'dev', '1']), two.index(['mmc', 'info']))

    def test_nothing_writes_loads_boots_runs_the_exporter_or_saves(self) -> None:
        for page in (1, 2):
            used = {t[0] for t in commands(script(page))}
            for forbidden in FORBIDDEN:
                self.assertNotIn(forbidden, used)
            self.assertNotRegex(script(page), r'0x[0-9a-fA-F]+|\$\(|`')
            code = '\n'.join(line for line in script(page).splitlines() if not line.lstrip().startswith(('#', 'echo')))
            self.assertNotRegex(code, r'loados|bootm|saveenv|exporter|write|erase|rescan|hwpartition|setdsr|read')

    def test_no_placeholder_is_left_and_each_page_fits_the_screen(self) -> None:
        for page in (1, 2):
            self.assertNotRegex(script(page), r'@[A-Z_0-9]+@')
            self.assertIn(f'PAGE {page} OF 2', script(page))
            self.assertLessEqual(len([x for x in script(page).splitlines() if x.strip().startswith('echo ')]), 14)

    def test_the_wait_and_reset_are_the_memory_diagnostics(self) -> None:
        memory = memory_script((ROOT / 'boot/diag/memory/u-boot-script.txt').read_text(), '', HEADER, 1)
        for page in (1, 2):
            self.assertEqual(script(page).splitlines()[-9:], memory.splitlines()[-9:])


class Package(unittest.TestCase):
    def test_each_image_is_u_boot_plus_one_script_and_nothing_else(self) -> None:
        u_boot = diag_u_boot(BL33, DWELL)
        self.assertEqual(u_boot, bl33_patch(BL33, diag_env_edits(DWELL)))
        for page in (1, 2):
            image = diag_bl33(u_boot, script(page))
            self.assertTrue(image.startswith(u_boot))
            self.assertLess(len(image), 0x100000)

    def test_the_entries_are_apart_from_every_other(self) -> None:
        others = {'ios', 'ios-diag', 'ios-diag-loader', 'ios-diag-memory', 'ios-diag-memory-fdt'}
        self.assertEqual(len({p.entry for p in PAGES} | others), 7)
        self.assertEqual(len({p.ident for p in PAGES} | {'iDiag', 'iDiagL', 'iDiagM', 'iDiagMF'}), 6)
        for page in PAGES:
            self.assertIn(f'boot_prefixes=/switchroot/{page.entry}/', ini(page))


class Requirements(unittest.TestCase):
    def test_a_synthetic_sdhc_card_meets_what_mmc_info_can_show(self) -> None:
        rows = judge(SYNTHETIC_INFO)
        self.assertEqual({v for _, v, _ in rows[:5]}, {MET})
        self.assertEqual({v for _, v, _ in rows[5:]}, {NOT_SHOWN})

    def test_each_condition_fails_on_its_own_value(self) -> None:
        for old, new, row in (
            ('Rd Block Len: 512', 'Rd Block Len: 1024', 2),
            ('High Capacity: Yes', 'High Capacity: No', 1),
            ('Bus Width: 4-bit', 'Bus Width: 8-bit', 3),
            ('Bus Width: 4-bit', 'Bus Width: 4-bit DDR', 3),
            ('SD version 3.0', 'MMC version 5.1', 0),
        ):
            self.assertEqual(judge(SYNTHETIC_INFO.replace(old, new))[row][1], NOT_MET, new)

    def test_a_missing_line_is_not_shown_rather_than_met(self) -> None:
        self.assertEqual(judge('')[0][1], NOT_SHOWN)
        self.assertNotIn(MET, {v for _, v, _ in judge('')})

    def test_mask_hides_maker_oem_and_name_and_keeps_the_rest(self) -> None:
        text = mask(SYNTHETIC_INFO)
        for hidden in ('5344', 'SL32G', 'Manufacturer ID: 3'):
            self.assertNotIn(hidden, text)
        self.assertIn('Rd Block Len: 512', text)
        self.assertEqual(judge(text)[:5], judge(SYNTHETIC_INFO)[:5])


if __name__ == '__main__':
    unittest.main()
