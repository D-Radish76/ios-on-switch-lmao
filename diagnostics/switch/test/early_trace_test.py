"""The early stage marks of --//kernel:early_trace: what start.s may touch, where it may write, and that only the trace
configuration changes. Reads the sources; nothing is built."""
import re
import unittest
from pathlib import Path



def source(path):
    return Path(path).read_text()


START = source("kernel/xnu-10063.141.1/osfmk/arm64/start.s")
DIAG = source("diagnostics/switch/SwitchDiag.c")
WATCHDOG = source("drivers/NativeSwitchWatchdog/NativeSwitchWatchdog.cpp")
FB, FB_BYTES, ROW = 0xF5A00000, 0x384000, 2880


def macro():
    board = START[START.index("#if defined(SWITCH_XNU_BOARD) && SWITCH_XNU_BOARD\n/* SWITCH_EARLY_TRACE"):]
    return board[board.index(".macro SWITCH_EARLY_MARK"):board.index(".endmacro")]


def block(stage):
    top = FB + (1279 - (16 + 40 * (stage - 1))) * ROW + 280 * 4
    return [top - ROW * row + 4 * word for row in range(16) for word in range(16)]


class StartMacro(unittest.TestCase):
    def test_only_the_scratch_registers_are_used(self):
        used = set(re.findall(r"\b[xw](\d+)\b", macro()))
        self.assertEqual(used - {"28", "21"}, {"16", "17", "18"})

    def test_x28_and_x21_are_only_tested(self):
        for line in macro().splitlines():
            if re.search(r"\bx(28|21)\b", line):
                self.assertRegex(line.strip(), r"^(cbz|cbnz)\b")

    def test_no_call_no_stack_and_no_flag_use_after_the_loop(self):
        body = macro()
        for forbidden in (r"\bbl\b", r"\bblr\b", r"\bsp\b", r"\bldr\b", r"\bmsr\b", r"\bmrs\b"):
            self.assertIsNone(re.search(forbidden, body), forbidden)

    def test_the_trace_only_widens_the_draw_condition(self):
        self.assertIn(".if (\\color != 0xff70ff70) || SWITCH_EARLY_TRACE", macro())
        self.assertIn("#ifndef SWITCH_EARLY_TRACE\n#define SWITCH_EARLY_TRACE 0\n#endif", START)

    def test_the_record_is_armed_for_the_c_stages_only_in_the_trace(self):
        at = START.index("#if SWITCH_EARLY_TRACE\n\tmov x17, #2\n\tstr x17, [x16]\n#endif")
        self.assertLess(START.rindex("mov x28, #1", 0, at), at)
        self.assertEqual(START.count("mov x17, #2\n\tstr x17, [x16]"), 1)

    def test_every_store_offset_is_inside_the_16_pixel_row(self):
        offsets = [int(o) for o in re.findall(r"str w17, \[x16, #(\d+)\]", macro())]
        self.assertEqual(offsets, list(range(0, 64, 4)))
        self.assertIn("sub x16, x16, #2880", macro())
        self.assertIn("mov x18, #16", macro())


class Addresses(unittest.TestCase):
    def test_every_block_of_stages_1_to_21_is_inside_the_framebuffer(self):
        for stage in range(1, 22):
            addresses = block(stage)
            self.assertEqual(len(set(addresses)), 256)
            self.assertGreaterEqual(min(addresses), FB, stage)
            self.assertLess(max(addresses) + 4, FB + FB_BYTES + 1, stage)
            self.assertEqual(max(addresses) % 4, 0)

    def test_blocks_do_not_overlap_each_other(self):
        seen = set()
        for stage in range(1, 22):
            addresses = set(block(stage))
            self.assertFalse(seen & addresses, stage)
            seen |= addresses

    def test_the_c_marks_use_the_same_pixels(self):
        for stage in (7, 21):
            pixels = {(1279 - (16 + 40 * (stage - 1) + dx)) * 720 + 280 + dy for dx in range(16) for dy in range(16)}
            self.assertEqual({FB + 4 * p for p in pixels}, set(block(stage)))

    def test_the_geometry_is_the_boot_contract(self):
        self.assertIn("cmp x16, #2880", START)
        self.assertIn("0xf5a00000", START)
        self.assertEqual(720 * 4, ROW)
        self.assertEqual(1280 * ROW, FB_BYTES)


class Wiring(unittest.TestCase):
    def test_the_c_marks_are_debug_only_and_need_the_trace_value(self):
        at = DIAG.index("early_trace_mark(unsigned stage)")
        self.assertIn("#if DEVELOPMENT || DEBUG", DIAG[:at][-400:])
        self.assertIn("if (switch_early_record[0] == 2)", DIAG)
        call = DIAG.index("early_trace_mark(stage)")
        self.assertIn("#if DEVELOPMENT || DEBUG", DIAG[:call][-200:])

    def test_the_mark_stays_before_the_framebuffer_map_is_retired(self):
        self.assertLess(DIAG.index("early_trace_mark(stage)"), DIAG.index("switch_early_record[0] = 0"))

    def test_the_flag_is_off_by_default_and_reaches_start_s_only(self):
        kernel = source("kernel/BUILD.bazel")
        self.assertRegex(kernel, r'name = "early_trace",\s+build_setting_default = False')
        objects = source("kernel/switch/objects.bzl")
        self.assertIn('start = "osfmk/arm64/start.s"', objects)
        self.assertEqual(objects.count("-DSWITCH_EARLY_TRACE=1"), 1)
        select = source("kernel/xnu-10063.141.1/config/xnu_objects.bzl")
        self.assertIn('"//bazel:release": switch_copts(ours, release.copts)', select)
        self.assertEqual(select.count("with_early_trace"), 3)
        for group in ("early_trace_debug", "early_trace_kprof_debug", "kprof_only_debug"):
            self.assertIn('"//bazel:debug"', kernel[kernel.index('name = "%s"' % group):][:200])

    def test_xnu_12377_is_untouched(self):
        self.assertNotIn("early_trace", source("kernel/xnu-12377.121.6/config/xnu_objects.bzl"))
        self.assertNotIn("SWITCH_EARLY_TRACE", source("kernel/xnu-12377.121.6/osfmk/arm64/start.s"))


class DevelopmentOnlyWrites(unittest.TestCase):
    """The DEVELOPMENT configuration adds WDT4 through the freeze heartbeat: audited here, not added to."""

    def test_wdt4_is_armed_only_from_watchdogds_connection_in_debug_kernels(self):
        depth, stack = 0, []
        for number, line in enumerate(WATCHDOG.splitlines(), 1):
            if line.startswith("#if"):
                stack.append("DEVELOPMENT || DEBUG" in line)
            elif line.startswith("#else"):
                stack[-1] = False
            elif line.startswith("#endif"):
                stack.pop()
            if "switch_heartbeat_arm" in line and not line.lstrip().startswith("//"):
                self.assertTrue(any(stack), number)
        self.assertEqual(stack, [])
        self.assertEqual(WATCHDOG.count("switch_heartbeat_arm("), 1)


if __name__ == "__main__":
    unittest.main()
