"""D0-S v2: its script, the inspection of its image, a real link, the four addresses and the entry.

The v2 script makes no load from a device address, and the image reads four words behind its allowlist, each between two
screen lines that were drawn and settled. D0-S (v1) and every other image must still be what
boot/diag/erista-diag-image-hashes.txt records.
"""

import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import ClassVar

import diag_images
from diag_mmio_audit import audit
from diag_loader import diag_script, entry_ini
from diag_loader_inspect import S2_ADDRESSES, inspect_disassembly, inspect_image, inspect_map

ROOT = Path('.')
LOADER = ROOT / 'boot/diag/loader'
HASHES = ROOT / 'boot/diag/erista-diag-image-hashes.txt'
HEADER = (ROOT / 'kits/SwitchBoard/SwitchBoard.h').read_text()
DTB_BYTES = len((ROOT / 'bazel/pinned/firmware/erista-sku0.dtb').read_bytes())
RENDERED = diag_script((ROOT / 'boot/diag/state/u-boot-script-state-v2.txt').read_text(), HEADER, 'erista', DTB_BYTES)
D0 = diag_script((ROOT / 'boot/diag/loader/u-boot-script.txt').read_text(), HEADER, 'erista', DTB_BYTES)
V1 = diag_script((ROOT / 'boot/diag/state/u-boot-script-state.txt').read_text(), HEADER, 'erista', DTB_BYTES)
LINES = [ln.strip() for ln in RENDERED.splitlines() if ln.strip() and not ln.strip().startswith('#')]
MAP = 'switch_diag_read32 switch_diag_exit switch_diag_after_transition probe_show_exception'
S2_MAP = (
    MAP + ' switch_s2_run_on_hardware switch_s2_run switch_s2_access switch_s2_read32 switch_s2_read16 switch_s2_width'
    ' switch_s2_may_read switch_s2_address switch_s2_settle'
)
SOURCES = ('s2.c', 's2.h', 's2_regs.c', 's2_gate.c', 's2_access.c', 's2_report.c', 's2_report.h', 's2_settle.c')
DEVICE_LITERAL = re.compile(r'0x[5-7][0-9a-f]{7}u?\b', re.IGNORECASE)


class Script(unittest.TestCase):
    def test_no_command_loads_from_a_device_address(self) -> None:
        self.assertNotIn('setexpr', RENDERED)
        for command in re.findall(r'\*0x[0-9a-fA-F]+', RENDERED):
            self.assertIn(command, ('*0x8D000000', '*0x8D000004'))
        for address in re.findall(r'0x[0-9a-fA-F]{8}', '\n'.join(LINES)):
            value = int(address, 16)
            self.assertFalse(0x50000000 <= value < 0x7F000000, address)

    def test_no_command_reaches_a_device_the_card_or_the_environment_store(self) -> None:
        text = '\n'.join(LINES)
        self.assertNotRegex(
            text, r'\b(mmc|usb|sf|nand|fatload|ext4load|load|tftp|dhcp|saveenv|env save|run|source|go|md|mw|mm|nm|cmp|i2c|crc32)\b '
        )

    def test_it_is_the_d0_script_plus_the_title_and_the_framebuffer_echo(self) -> None:
        d0 = [ln.strip() for ln in D0.splitlines() if ln.strip() and not ln.strip().startswith('#')]
        extra = [ln for ln in LINES if ln not in d0]
        self.assertEqual(2, len(extra), extra)
        self.assertTrue(extra[0].startswith('echoe "D0-S V2 STATE DIAGNOSTIC'))
        self.assertTrue(extra[1].startswith('echoe "D0-S V2 FRAMEBUFFER PROPERTIES: ADDRESS ${handoff_fb_addr}'))
        self.assertIn('sleep 5', LINES)
        for line in d0:
            if not line.startswith('echoe "D0 LOADER'):
                self.assertIn(line, LINES)

    def test_the_ram_reads_of_d0_are_kept(self) -> None:
        self.assertIn('itest.l *0x8D000000 == 0xedfe0dd0 && itest.l *0x8D000004 == 0x00000300', RENDERED)
        self.assertIn('fdt rsvmem add 0xF5A00000 0x400000', RENDERED)

    def test_the_framebuffer_properties_are_echoed_after_bootm_prep_and_before_the_handoff(self) -> None:
        code = '\n'.join(LINES)
        prep, echo, go = (code.index(s) for s in ('bootm prep', 'D0-S V2 FRAMEBUFFER PROPERTIES', 'bootm go'))
        self.assertLess(prep, echo)
        self.assertLess(echo, go)

    def test_the_script_ends_in_one_reset_and_hands_off_once(self) -> None:
        self.assertEqual(1, sum(1 for ln in LINES if ln == 'reset'))
        self.assertEqual(['reset', 'while true; do', 'sleep 1', 'done'], LINES[-4:])
        self.assertEqual(1, sum(1 for ln in LINES if ln == 'bootm go'))

    def test_the_v1_script_is_untouched(self) -> None:
        self.assertEqual(4, V1.count('*0x700b') + V1.count('*0x6000'))
        self.assertIn('setexpr.l state_car_reset *0x60006004', V1)

    def test_the_failure_statuses_are_in_the_run_sheet(self) -> None:
        sheet = (ROOT / 'ERISTA_D0S_V2_RUN.md').read_text()
        for status in set(re.findall(r'setenv handoff_status ([A-Z_]+)', RENDERED)) - {'READY'}:
            self.assertIn(status, sheet, status)


class Inspection(unittest.TestCase):
    def test_the_map_needs_the_s2_symbols_and_refuses_them_elsewhere(self) -> None:
        self.assertTrue(inspect_map(MAP, s2=True))
        self.assertEqual([], inspect_map(S2_MAP, s2=True))
        self.assertTrue(inspect_map(S2_MAP))

    def test_any_other_stage_in_an_s2_link_is_refused(self) -> None:
        for symbol in ('switch_t1_run', 'switch_t1r_read32', 'switch_t1v3_run', 'switch_t1t_run', 'switch_t2_run', 'switch_s2_write32'):
            self.assertTrue(inspect_map(f'{S2_MAP} {symbol}', s2=True), symbol)

    def test_the_image_holds_exactly_the_four_register_words_once(self) -> None:
        def flat(words: list[int]) -> bytes:
            return b''.join(w.to_bytes(4, 'little') for w in words)

        listed = sorted(S2_ADDRESSES)
        self.assertEqual([0x60006004, 0x60006010, 0x700B0024, 0x700B002C], listed)
        self.assertEqual([], inspect_image(flat(listed), s2=True))
        self.assertTrue(inspect_image(flat([*listed, 0x700B0020]), s2=True))
        self.assertTrue(inspect_image(flat([*listed, 0x60006008]), s2=True))
        self.assertTrue(inspect_image(flat(listed[1:]), s2=True))
        self.assertTrue(inspect_image(flat([*listed, listed[0]]), s2=True))

    def test_the_raw_reads_are_one_word_load_and_one_halfword_load_and_only_the_gate_calls_them(self) -> None:
        def function(name: str, *lines: str) -> str:
            body = '\n'.join(f'  {0x100 + 4 * i:x}: {line}' for i, line in enumerate(lines))
            return f'0000000000000100 <{name}>:\n{body}\n'

        good = (
            function('switch_diag_read32', 'ldr w0, [x0]', 'ret')
            + function('switch_s2_read32', 'ldr w0, [x0]', 'ret')
            + function('switch_s2_read16', 'mov w8, w0', 'ldrh w0, [x8]', 'ret')
            + function('switch_s2_access', 'bl 0x100 <switch_s2_read32>', 'bl 0x100 <switch_s2_read16>')
        )
        self.assertEqual([], [p for p in inspect_disassembly(good, s2=True) if 'switch_s2' in p])
        for raw in ('switch_s2_read32', 'switch_s2_read16'):
            rogue = good + function('rogue', f'b 0x100 <{raw}>')
            self.assertTrue([p for p in inspect_disassembly(rogue, s2=True) if p.startswith('rogue: calls')], raw)
        stored = good.replace('ldr w0, [x0]\n  104: ret', 'str w1, [x0]\n  104: ret', 1)
        self.assertTrue(inspect_disassembly(stored, s2=True))

    def test_a_width_that_is_not_the_listed_one_is_refused(self) -> None:
        def function(name: str, *lines: str) -> str:
            body = '\n'.join(f'  {0x100 + 4 * i:x}: {line}' for i, line in enumerate(lines))
            return f'0000000000000100 <{name}>:\n{body}\n'

        def build(read32: str, read16: str) -> str:
            return (
                function('switch_diag_read32', 'ldr w0, [x0]', 'ret')
                + function('switch_s2_read32', read32, 'ret')
                + function('switch_s2_read16', read16, 'ret')
                + function('switch_s2_access', 'bl 0x100 <switch_s2_read32>', 'bl 0x100 <switch_s2_read16>')
            )

        ok = build('ldr w0, [x0]', 'ldrh w0, [x0]')
        self.assertEqual([], [p for p in inspect_disassembly(ok, s2=True) if 'switch_s2' in p])
        wide16 = [p for p in inspect_disassembly(build('ldr w0, [x0]', 'ldr w0, [x0]'), s2=True) if 'switch_s2_read16' in p]
        self.assertTrue(wide16)
        narrow32 = [p for p in inspect_disassembly(build('ldrh w0, [x0]', 'ldrh w0, [x0]'), s2=True) if 'switch_s2_read32' in p]
        self.assertTrue(narrow32)
        byte16 = [p for p in inspect_disassembly(build('ldr w0, [x0]', 'ldrb w0, [x0]'), s2=True) if 'switch_s2_read16' in p]
        self.assertTrue(byte16)


class Sources(unittest.TestCase):
    def test_the_allowlist_is_four_addresses_with_their_widths_and_only_s2_regs_holds_them(self) -> None:
        text = (LOADER / 's2_regs.c').read_text()
        self.assertEqual(
            [('S2_CAR_RST_DEVICES_L', '0x60006004', '32'), ('S2_CAR_CLK_OUT_ENB_L', '0x60006010', '32'),
             ('S2_SDMMC1_PRESENT_STATE', '0x700b0024', '32'), ('S2_SDMMC1_CLOCK_CONTROL', '0x700b002c', '16')],
            re.findall(r'\[(S2_\w+)\] = \{(0x[0-9a-f]{8})u, S2_WIDTH_(\d+)\}', text),
        )  # fmt: skip
        self.assertEqual(4, len(re.findall(r'0x[0-9a-f]{8}u', text)))
        for name in ('0x700b0020', '0x700b002e', '0x700b002f'):
            for source in SOURCES:
                self.assertNotIn(name, (LOADER / source).read_text().lower(), source)
        self.assertIn('width == table[reg].width', text)
        for name in (*SOURCES, 'diag.c'):
            if name == 's2_regs.c':
                continue
            lines = [ln for ln in (LOADER / name).read_text().splitlines() if DEVICE_LITERAL.search(ln)]
            lines = [ln for ln in lines if not (name == 'diag.c' and ('0x70000804' in ln or '0x7000f9d8' in ln))]
            self.assertEqual([], lines, name)

    def test_there_is_no_write_to_a_device_in_any_s2_file(self) -> None:
        for name in SOURCES:
            text = (LOADER / name).read_text()
            self.assertNotRegex(text, r'write32|may_write|io->write', name)
        sites = [
            ln.strip() for ln in (LOADER / 's2_access.c').read_text().splitlines() if '(uintptr_t)' in ln and 'volatile' in ln
        ]
        self.assertEqual(
            [
                'return *(const volatile uint32_t *)(uintptr_t)address;',
                'return *(const volatile uint16_t *)(uintptr_t)address;',
            ],
            sites,
        )
        gate = (LOADER / 's2_gate.c').read_text()
        self.assertLess(gate.index('switch_s2_may_read(reg, width)'), gate.index('switch_s2_read16(address)'))
        self.assertIn('width == S2_WIDTH_16 ? switch_s2_read16(address) : switch_s2_read32(address)', gate)

    def test_every_read_is_preceded_by_a_shown_begin_and_followed_by_a_shown_result(self) -> None:
        text = (LOADER / 's2.c').read_text()
        step = text[text.index('static int step_read(') : text.index('static void finish(')]
        self.assertLess(step.index('shown(io, result, begin)'), step.index('get(io, result, reg)'))
        self.assertLess(step.index('get(io, result, reg)'), step.index('shown(io, result, begin + 1)'))
        self.assertEqual(1, len(re.findall(r'\bget\(io, result, reg\)', text)))

    def test_the_sdmmc1_reads_are_behind_the_gate_in_the_run(self) -> None:
        text = (LOADER / 's2.c').read_text()
        run = text[text.index('void switch_s2_run(') :]
        gate = run.index('S2_STEP_GATE')
        for word in ('S2_SDMMC1_PRESENT_STATE', 'S2_SDMMC1_CLOCK_CONTROL'):
            self.assertGreater(run.index(word), gate)
            self.assertEqual(1, text.count(word))
        self.assertIn('result->sd_state == SWITCH_S2_SD_READ', run)

    def test_the_screen_callback_draws_then_settles_and_refuses_without_a_screen(self) -> None:
        text = (LOADER / 'diag.c').read_text()
        show = text[text.index('static int show_s2(') : text.index('static void report_s2(')]
        self.assertLess(show.index('!diag.screen'), show.index('put(LEFT, 9 + step'))
        self.assertLess(show.index('put(LEFT, 9 + step'), show.index('return switch_s2_settle()'))
        self.assertIn('return 0;', show[: show.index('put(')])

    def test_the_settle_has_a_barrier_a_bounded_wait_and_no_event_wait(self) -> None:
        text = (LOADER / 's2_settle.c').read_text()
        self.assertIn('dsb sy', text)
        self.assertIn('SETTLE_ITERATION_LIMIT', text)
        for word in ('wfe', 'wfi', 'smc', 'msr'):
            self.assertNotIn(word, text)

    def test_nothing_of_the_other_stages_and_no_other_peripheral(self) -> None:
        for name in SOURCES:
            text = (LOADER / name).read_text().lower()
            for word in ('t1v3', 't1t_', 't2_', 'i2c', 'pwm_csr', 'regulator', 'gpio_out', 'pmc_', 'wdt', 'thermtrip', 'retry('):
                self.assertNotIn(word, text, f'{name} mentions {word}')

    def test_no_em_dash_in_the_new_files(self) -> None:
        for name in SOURCES:
            self.assertNotIn('—', (LOADER / name).read_text())
        self.assertNotIn('—', (ROOT / 'boot/diag/state/u-boot-script-state-v2.txt').read_text())


class SavedAudit(unittest.TestCase):
    def test_the_saved_static_audit_passed_on_the_four_words_and_nothing_else(self) -> None:
        text = (ROOT / 'boot/diag/erista-s2-mmio-audit.txt').read_text()
        self.assertIn('AUDIT PASSED', text)
        self.assertIn('6. Flat image: register address words match the D0-S v2 list', text)
        self.assertIn('switch_s2_read32: ldr w0, [x8]; called by switch_s2_access', text)
        self.assertIn('switch_s2_read16: ldrh w0, [x8]; called by switch_s2_access', text)
        self.assertIn('AUDIT PASSED (D0-S v2): the image holds exactly 0x60006004, 0x60006010, 0x700b0024 and 0x700b002c', text)
        self.assertNotIn('only the two identity registers reach a call', text)
        self.assertNotIn('PROBLEMS', text)
        section2 = text[text.index('2. Where'):text.index('3. The path')]
        self.assertEqual(['0x70000804', '0x7000f9d8'], re.findall(r'with x0=(0x[0-9a-f]+)', section2))


class Package(unittest.TestCase):
    def test_the_entry_is_its_own_and_boots_nothing(self) -> None:
        ini = entry_ini('iOS Diag State V2', 'ios-diag-state-v2', 'iDiagS2')
        self.assertIn('[iOS Diag State V2]', ini)
        self.assertIn('boot_prefixes=/switchroot/ios-diag-state-v2/', ini)
        for other in ('iDiagS\n', 'ios-diag-state/', 'ios-diag-loader/'):
            self.assertNotIn(other, ini)
        self.assertNotIn('boot=1', ini)

    def test_the_bazel_target_is_separate_from_d0s(self) -> None:
        text = (ROOT / 'boot/BUILD.bazel').read_text()
        block = text[text.index('name = "diag_state_v2"') :]
        block = block[: block.index('\n)\n')]
        for expected in (
            'entry = "ios-diag-state-v2"',
            'ident = "iDiagS2"',
            'title = "iOS Diag State V2"',
            's2 = True',
            '-DSWITCH_DIAG_S2=1',
            'u-boot-script-state-v2.txt',
        ):
            self.assertIn(expected, block)
        self.assertNotIn('u-boot-script-state.txt', block)
        for flag in ('t1 = True', 't1r = True', 't1t = True', 't1v3 = True', 't2 = True', 'reservations'):
            self.assertNotIn(flag, block)
        sources = block[block.index('srcs = [') : block.index('hdrs = [')]
        for source in diag_images.S2:
            self.assertIn(f'":{source}"', sources)
        v1 = text[text.index('name = "diag_state",') :]
        v1 = v1[: v1.index('\n)\n')]
        self.assertIn('entry = "ios-diag-state"', v1)
        self.assertNotIn('s2', v1.replace('"ios-diag-state"', ''))

    def test_no_s2_code_is_in_the_other_images(self) -> None:
        for name, (sources, defines, _flag) in diag_images.IMAGES.items():
            if name == 'diag_state_v2':
                continue
            self.assertFalse([s for s in sources if 's2' in s], name)
            self.assertNotIn('-DSWITCH_DIAG_S2=1', defines)

    def test_the_psci_reset_path_is_the_shared_exit(self) -> None:
        sources = diag_images.IMAGES['diag_state_v2'][0]
        self.assertIn('diag/loader/exit.c', sources)
        exit_text = (LOADER / 'exit.c').read_text()
        self.assertIn('SWITCH_DIAG_PSCI_SYSTEM_RESET', exit_text)
        self.assertEqual(1, exit_text.count('smc #0'))
        self.assertIn('switch_diag_exit(say);', (LOADER / 'diag.c').read_text())


@unittest.skipUnless(
    shutil.which('clang') and shutil.which('ld.lld') and shutil.which('llvm-objdump') and shutil.which('llvm-objcopy'),
    'no LLVM on PATH',
)
class RealLink(unittest.TestCase):
    FLAGS: ClassVar[list[str]] = [*diag_images.COPTS, '-DSWITCH_DIAG_S2=1']
    INCLUDES: ClassVar[list[str]] = [f'boot/{d}' for d in diag_images.INCLUDES] + ['kits/SwitchBoard']

    def link(self, scratch: str, extra: list[str]) -> tuple[str, str, bytes]:
        sources = [*(f'boot/{s}' for s in diag_images.COMMON + diag_images.S2 + diag_images.TAIL), *extra]
        objects = []
        for index, source in enumerate(sources):
            obj = f'{scratch}/{index}.o'
            command = ['clang', *self.FLAGS, *(f for inc in self.INCLUDES for f in ('-I', inc)), '-c', source, '-o', obj]
            result = subprocess.run(command, capture_output=True, text=True, check=False)
            if result.returncode and 'unable to create target' in result.stderr:
                self.skipTest('this clang does not target AArch64')
            self.assertEqual(0, result.returncode, result.stderr)
            objects.append(obj)
        elf, linkmap, flat = f'{scratch}/s2.elf', f'{scratch}/s2.map', f'{scratch}/s2.bin'
        linked = subprocess.run(
            ['ld.lld', '-T', 'boot/diag/loader/link.ld', f'-Map={linkmap}', '-o', elf, *objects],
            capture_output=True, text=True, check=False,
        )  # fmt: skip
        self.assertEqual(0, linked.returncode, linked.stderr)
        dis = subprocess.run(['llvm-objdump', '-d', '--no-show-raw-insn', elf], capture_output=True, text=True, check=True).stdout
        subprocess.run(['llvm-objcopy', '-O', 'binary', elf, flat], check=True)
        return Path(linkmap).read_text(), dis, Path(flat).read_bytes()

    def test_it_links_and_passes_the_s2_inspection(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            text, dis, flat = self.link(scratch, [])
            self.assertEqual([], inspect_map(text, s2=True))
            self.assertEqual([], inspect_disassembly(dis, s2=True))
            self.assertEqual([], inspect_image(flat, s2=True))
            self.assertTrue(inspect_map(text))

    def test_the_linked_code_has_one_psci_smc(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            _, dis, _ = self.link(scratch, [])
            self.assertEqual(1, len(re.findall(r'\bsmc\b', dis)))

    def test_the_static_audit_passes_and_refuses_a_width_that_was_changed(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            _, dis, flat = self.link(scratch, [])
            report, problems = audit(dis, flat, s2=True)
            self.assertEqual([], problems)
            self.assertIn('6. Flat image: register address words match the D0-S v2 list', report)
            self.assertTrue(any('switch_s2_read16: ldrh w0' in line for line in report))
            body = dis[dis.index('<switch_s2_read16>:') :]
            tampered = dis.replace(body[: body.index('ret')], body[: body.index('ret')].replace('ldrh', 'ldr'), 1)
            self.assertNotEqual(dis, tampered)
            self.assertTrue([p for p in audit(tampered, flat, s2=True)[1] if 'switch_s2_read16' in p])
            head = dis[: dis.index('<switch_s2_read32>:')]
            tail = dis[dis.index('<switch_s2_read32>:') :]
            word = head + tail.replace('ldr\tw0, [x8]\n', 'ldrh\tw0, [x8]\n', 1)
            self.assertNotEqual(dis, word)
            self.assertTrue([p for p in audit(word, flat, s2=True)[1] if 'switch_s2' in p])

    def test_a_register_address_added_by_another_file_is_refused_in_the_linked_image(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            extra = Path(scratch) / 'extra.c'
            extra.write_text('const unsigned extra_address = 0x700b0020u;\n')
            _, _, flat = self.link(scratch, [str(extra)])
            self.assertTrue([p for p in inspect_image(flat, s2=True) if '0x700b0020' in p])

    def test_a_second_caller_of_the_raw_read_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            rogue = Path(scratch) / 'rogue.c'
            rogue.write_text(
                '#include <stdint.h>\nuint32_t switch_s2_read32(uint32_t);\n'
                'uint32_t rogue(void) { return switch_s2_read32(0x700b0020u); }\n'
            )
            _, dis, _ = self.link(scratch, [str(rogue)])
            self.assertTrue([p for p in inspect_disassembly(dis, s2=True) if p.startswith('rogue: calls')])

    def test_every_recorded_hash_is_what_this_clang_builds(self) -> None:
        lines = HASHES.read_text().splitlines()
        current = subprocess.run(['clang', '--version'], capture_output=True, text=True, check=True).stdout.splitlines()[0]
        if current != lines[0].removeprefix('clang: '):
            self.skipTest('hashes were recorded with another clang')
        recorded = {f[0]: f[1] for f in (ln.split() for ln in lines[1:]) if len(f) >= 2}
        self.assertEqual(set(diag_images.IMAGES), set(recorded))
        tools = diag_images.argparse.Namespace(clang='clang', ld_lld='ld.lld', objcopy='llvm-objcopy', objdump='llvm-objdump')
        for name in diag_images.IMAGES:
            with tempfile.TemporaryDirectory() as scratch:
                digest, problems = diag_images.build(name, tools, Path(scratch))
            self.assertEqual([], problems, name)
            self.assertEqual(recorded[name], digest, name)


if __name__ == '__main__':
    unittest.main()
