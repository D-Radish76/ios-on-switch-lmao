"""ERISTA_XNU_BOOT_STAGES.md, ERISTA_IOKIT_DEPENDENCIES.md and ERISTA_PORT_ROADMAP.md against the source lines they cite."""

import re
import unittest
from pathlib import Path

XNU = Path('kernel/xnu-10063.141.1')
DOCS = ('ERISTA_XNU_BOOT_STAGES.md', 'ERISTA_IOKIT_DEPENDENCIES.md', 'ERISTA_PORT_ROADMAP.md')

STAGE_CALLS = {
    7: ('osfmk/arm/arm_init.c', 360), 8: ('osfmk/arm/arm_init.c', 364), 9: ('osfmk/arm/arm_init.c', 378),
    10: ('osfmk/arm/arm_init.c', 451), 11: ('osfmk/arm/arm_init.c', 481), 12: ('osfmk/arm/arm_init.c', 484),
    13: ('osfmk/arm/arm_init.c', 488), 14: ('osfmk/arm/arm_init.c', 496), 15: ('osfmk/arm/arm_init.c', 499),
    16: ('osfmk/arm/arm_init.c', 547), 17: ('osfmk/arm64/arm_vm_init.c', 1882), 18: ('osfmk/arm64/arm_vm_init.c', 1888),
    19: ('osfmk/arm64/arm_vm_init.c', 2159), 20: ('osfmk/arm64/arm_vm_init.c', 2162), 21: ('osfmk/arm64/arm_vm_init.c', 2168),
}  # fmt: skip

CITED_LINES = (
    ('bsd/kern/bsd_init.c', 699, 'IOKitInitializeTime'), ('bsd/kern/bsd_init.c', 808, 'bsd_autoconf'),
    ('bsd/kern/bsd_init.c', 809, 'switch_boot_autoconf_done'), ('bsd/kern/bsd_init.c', 898, 'vfs_mountroot'),
    ('bsd/kern/bsd_init.c', 1098, 'vm_init_before_launchd'), ('bsd/kern/bsd_init.c', 1110, 'load_init_program'),
    ('iokit/bsddev/IOKitBSDInit.cpp', 795, 'waitForMatchingService'), ('iokit/bsddev/IOKitBSDInit.cpp', 823, 'root-matching'),
    ('iokit/Kernel/IOStartIOKit.cpp', 93, 'switch_pmic_start'), ('osfmk/kern/startup.c', 821, 'PE_init_iokit'),
    ('osfmk/arm/arm_init.c', 548, 'arm_vm_init'), ('osfmk/arm/arm_init.c', 688, 'machine_startup'),
    ('osfmk/arm/rtclock.c', 85, 'rtclock_early_init'), ('osfmk/arm64/cpu.c', 734, 'cpu_init'),
    ('osfmk/arm64/machine_routines.c', 998, 'ml_parse_cpu_topology'),
)  # fmt: skip


def line(path: Path, number: int) -> str:
    return path.read_text(errors='replace').splitlines()[number - 1]


class Stages(unittest.TestCase):
    def test_every_stage_mark_is_where_the_document_puts_it(self) -> None:
        for stage, (name, number) in STAGE_CALLS.items():
            self.assertIn(f'switch_diag_early_stage({stage},', line(XNU / name, number), f'stage {stage}')
        start = (XNU / 'osfmk/arm64/start.s').read_text()
        for stage in range(1, 7):
            self.assertRegex(start, rf'SWITCH_EARLY_MARK {stage}\b', f'stage {stage}')

    def test_the_cited_call_sites_exist(self) -> None:
        for name, number, text in CITED_LINES:
            self.assertIn(text, line(XNU / name, number), f'{name}:{number}')
        self.assertIn('switch_block_publish', line(Path('drivers/NativeSwitchBlock/NativeSwitchBlock.cpp'), 4725))

    def test_the_stage_table_has_all_21_rows(self) -> None:
        text = Path('ERISTA_XNU_BOOT_STAGES.md').read_text()
        self.assertEqual(list(range(1, 22)), [int(n) for n in re.findall(r'^\| (\d+) \| ', text, re.MULTILINE)][:21])


class Documents(unittest.TestCase):
    def test_marks_limits_and_no_dashes(self) -> None:
        for name in DOCS:
            text = Path(name).read_text()
            self.assertRegex(text, r'\*\*unknown\*\*', name)
            self.assertRegex(text, r'\*\*confirmed\*\*', name)
            self.assertNotRegex(text, '[\\u2013\\u2014]', name)
            self.assertNotRegex(text, '(?i)ioswitch', name)
            self.assertLess(len(text.splitlines()), 500, name)
        for name in DOCS[:2]:
            self.assertIn('on HOLD', Path(name).read_text(), name)
        self.assertIn('HOLD', Path(DOCS[2]).read_text())


if __name__ == '__main__':
    unittest.main()
