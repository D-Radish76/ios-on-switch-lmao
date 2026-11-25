"""hush_lint.py: the line that broke page 1 on the Erista, the other characters hush may not take, and every script."""

import re
import unittest
from pathlib import Path

from diag_boot import DISTRO_BOOTCMD_DIAG, DWELL
from diag_memory import memory_script
from hush_lint import lint, lint_line

ROOT = Path('.')
HEADER = (ROOT / 'kits/SwitchBoard/SwitchBoard.h').read_text()
SCRIPTS = (
    'boot/u-boot-script.txt',
    'boot/diag/u-boot-diag-script.txt',
    'boot/diag/loader/u-boot-script.txt',
    'boot/diag/memory/u-boot-script.txt',
    'boot/diag/memory/page-1-bdinfo.txt',
    'boot/diag/memory/page-2-fdt.txt',
)
BROKEN_ON_THE_ERISTA = "echo U-Boot's own view: DRAM banks, relocaddr, reloc off, irq_sp, sp start, TLB addr, fdt_blob"


class Lines(unittest.TestCase):
    def test_the_line_that_failed_on_the_erista_is_rejected(self) -> None:
        self.assertEqual(len(lint_line(BROKEN_ON_THE_ERISTA)), 1)

    def test_each_character_hush_may_not_take_is_rejected_in_an_echo(self) -> None:
        for bad in (
            "it's",
            'say "x"',
            'a;b',
            'a&b',
            'a|b',
            'a<b',
            'a>b',
            'a`b',
            'a\\b',
            '$name',
            'a!b',
            'a*b',
            'a?b',
            'a{b}',
            'a#b',
            'a~b',
        ):
            with self.subTest(bad=bad):
                self.assertEqual(len(lint_line(f'    echo {bad}')), 1)

    def test_non_ascii_and_control_characters_are_rejected(self) -> None:
        for bad in ('echo café', 'echo a\x01b', 'echo —', 'setenv a b' + chr(0xA0) + 'c'):
            self.assertEqual(len(lint_line(bad)), 1, bad)

    def test_a_quote_or_brace_left_open_on_any_line_is_rejected(self) -> None:
        for bad in ("setenv a 'b", 'setenv a "b', 'setenv a ${b', 'echoe "abc', 'echoe abc', 'echoe "a" "b"'):
            self.assertEqual(len(lint_line(bad)), 1, bad)

    def test_what_the_scripts_use_is_accepted(self) -> None:
        for good in (
            'echo',
            'echo [/reserved-memory] an error here means the node is absent',
            'echo board t210b01=${t210b01} sku=${sku} status=${diag_status} console=${diag_console}',
            'echo INCOMPLETE: ${diag_reads} of @COUNT_HEX@ read (hex), status=${diag_status}',
            'echoe "REFUSED: ${handoff_status}. Reset in @DWELL@ s, expected to land in hekate"',
            'if test "${diag_status}" = "READY"; then',
            "# a comment may say what it likes, it's ignored",
            '',
        ):
            self.assertEqual(lint_line(good), [], good)


class Blocks(unittest.TestCase):
    def test_unbalanced_blocks_are_rejected(self) -> None:
        for bad in (
            'if true; then\n    echo a\n',
            'echo a\nfi\n',
            'for a in 1; do\nfi\n',
            'while true; do\n    sleep 1\nfi\n',
            'if true; then\ndone\n',
        ):
            self.assertNotEqual(lint(bad), [], bad)

    def test_nested_blocks_are_accepted(self) -> None:
        self.assertEqual(lint('if true; then\n    for a in 1; do\n        echo x\n    done\nfi\n'), [])

    def test_a_keyword_inside_echo_text_is_not_a_block(self) -> None:
        self.assertEqual(lint('echo if then fi done\n'), [])


class Scripts(unittest.TestCase):
    def test_every_script_template_is_clean(self) -> None:
        for name in SCRIPTS:
            with self.subTest(script=name):
                self.assertEqual(lint((ROOT / name).read_text()), [])

    def test_the_rendered_memory_pages_are_clean(self) -> None:
        frame = (ROOT / SCRIPTS[3]).read_text()
        for page, body in ((1, SCRIPTS[4]), (2, SCRIPTS[5])):
            text = memory_script(frame, (ROOT / body).read_text(), HEADER, page)
            self.assertEqual(lint(text), [], f'page {page}')

    def test_page_one_has_no_quote_character_at_all(self) -> None:
        text = (ROOT / SCRIPTS[4]).read_text()
        self.assertNotRegex(text, r'[\'"`\\]')

    def test_the_commands_the_image_runs_when_the_script_fails_are_clean(self) -> None:
        text = (DISTRO_BOOTCMD_DIAG % (DWELL, DWELL)).decode()
        for command in re.split(r';', text.split('=', 1)[1]):
            if command.strip().startswith('echo'):
                self.assertEqual(lint_line(command), [], command)


if __name__ == '__main__':
    unittest.main()
