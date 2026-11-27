"""Builds each diagnostic loader image on the host and prints the SHA-256 of the flat binary.

    diag_images.py [--clang CLANG] [--ld-lld LLD] [--objcopy OBJCOPY] [--objdump OBJDUMP] [--only NAME ...]

The sources, include directories, defines and flags are those of //boot:diag_loader and its siblings. It shows that a
change to a shared source leaves the other diagnostics' images byte for byte as they were: run it before and after and
compare the lines. A hash is only comparable with another from the same clang, which the first line records.
"""

import argparse
import hashlib
import subprocess
import sys
import tempfile
from pathlib import Path

from diag_loader_inspect import inspect_disassembly, inspect_image, inspect_map

BOOT = Path('boot')
INCLUDES = ('diag/loader', 'diag/sd_loader', 'handoff', 'loader', 'storage')
BOARD_INCLUDE = Path('kits/SwitchBoard')
COPTS = (
    '--target=aarch64-none-elf',
    '-march=armv8-a',
    '-mstrict-align',
    '-mgeneral-regs-only',
    '-ffreestanding',
    '-fno-builtin',
    '-fno-stack-protector',
    '-fno-pic',
    '-O2',
    '-Wall',
    '-Wextra',
    '-Werror',
    '-DPROBE_QUIET=1',
)
COMMON = ('diag/loader/entry.S', 'diag/loader/diag.c', 'diag/loader/checks.c', 'diag/loader/report.c')
TAIL = ('diag/loader/exit.c', 'loader/memory.c')
T1 = (
    'diag/loader/t1.c',
    'diag/loader/t1_regs.c',
    'diag/loader/t1_gate.c',
    'diag/loader/t1_report.c',
    'diag/loader/t1_access.c',
)
T1R = (
    'diag/loader/t1r.c',
    'diag/loader/t1r_regs.c',
    'diag/loader/t1r_gate.c',
    'diag/loader/t1r_report.c',
    'diag/loader/t1r_access.c',
)
T1V3 = (
    'diag/loader/t1v3.c',
    'diag/loader/t1v3_regs.c',
    'diag/loader/t1v3_gate.c',
    'diag/loader/t1v3_report.c',
    'diag/loader/t1v3_access.c',
)
T1T = (
    'diag/loader/t1t.c',
    'diag/loader/t1t_regs.c',
    'diag/loader/t1t_gate.c',
    'diag/loader/t1t_report.c',
    'diag/loader/t1t_access.c',
)
T2 = (
    'diag/t2/t2.c',
    'diag/loader/t2_image.c',
    'diag/loader/t2_prelude.c',
    'diag/loader/t2_bus.c',
    'diag/loader/t2_regs.c',
    'diag/loader/t2_gate.c',
    'diag/loader/t2_report.c',
    'diag/loader/t2_access.c',
)
S2 = (
    'diag/loader/s2.c',
    'diag/loader/s2_regs.c',
    'diag/loader/s2_gate.c',
    'diag/loader/s2_report.c',
    'diag/loader/s2_settle.c',
    'diag/loader/s2_access.c',
)
T4R = (
    'diag/loader/t4r.c',
    'diag/loader/t4r_regs.c',
    'diag/loader/t4r_gate.c',
    'diag/loader/t4r_report.c',
    'diag/loader/t4r_settle.c',
    'diag/loader/t4r_access.c',
)
T3R = (
    'diag/loader/t3r.c',
    'diag/loader/t3r_regs.c',
    'diag/loader/t3r_gate.c',
    'diag/loader/t3r_report.c',
    'diag/loader/t3r_settle.c',
    'diag/loader/t3r_access.c',
)
RESERVATIONS = ('diag/loader/reservations.c',)
SD = ('diag/sd_loader/sd_report.c', 'storage/sd_reference.c')

# name: (sources, defines, inspection flag)
IMAGES = {
    'diag_loader': (COMMON + TAIL, (), None),
    'diag_loader_r': (COMMON + RESERVATIONS + TAIL, ('-DSWITCH_DIAG_RESERVATIONS=1',), None),
    'diag_loader_sd': (
        COMMON + RESERVATIONS + SD + TAIL,
        ('-DSWITCH_DIAG_RESERVATIONS=1', '-DSWITCH_DIAG_SD=1'),
        'sd',
    ),
    'diag_t1': (COMMON + T1 + TAIL, ('-DSWITCH_DIAG_T1=1',), 't1'),
    'diag_t1r': (COMMON + T1R + TAIL, ('-DSWITCH_DIAG_T1R=1',), 't1r'),
    'diag_t1v3': (COMMON + T1V3 + TAIL, ('-DSWITCH_DIAG_T1V3=1',), 't1v3'),
    'diag_t1t': (COMMON + T1T + TAIL, ('-DSWITCH_DIAG_T1T=1',), 't1t'),
    'diag_t2': (COMMON + T2 + TAIL, ('-DSWITCH_DIAG_T2=1',), 't2'),
    'diag_state_v2': (COMMON + S2 + TAIL, ('-DSWITCH_DIAG_S2=1',), 's2'),
    'diag_t4r': (COMMON + T4R + TAIL, ('-DSWITCH_DIAG_T4R=1',), 't4r'),
    'diag_t3r': (COMMON + T3R + TAIL, ('-DSWITCH_DIAG_T3R=1',), 't3r'),
}


def build(name: str, tools: argparse.Namespace, scratch: Path) -> tuple[str, list[str]]:
    sources, defines, flag = IMAGES[name]
    includes = [f'-I{BOOT / d}' for d in INCLUDES] + [f'-I{BOARD_INCLUDE}']
    objects = []
    for index, source in enumerate(sources):
        if not (BOOT / source).exists():
            raise SystemExit(f'{name}: {BOOT / source} does not exist')
        obj = scratch / f'{name}-{index}.o'
        subprocess.run([tools.clang, *COPTS, *defines, *includes, '-c', str(BOOT / source), '-o', str(obj)], check=True)
        objects.append(obj)
    elf, linkmap, flat = scratch / f'{name}.elf', scratch / f'{name}.map', scratch / f'{name}.bin'
    subprocess.run(
        [tools.ld_lld, '-T', str(BOOT / 'diag/loader/link.ld'), f'-Map={linkmap}', '-o', str(elf), *map(str, objects)],
        check=True,
        capture_output=True,
    )
    subprocess.run([tools.objcopy, '-O', 'binary', str(elf), str(flat)], check=True)
    dis = subprocess.run(
        [tools.objdump, '-d', '--no-show-raw-insn', str(elf)], check=True, capture_output=True, text=True
    )
    flags = {flag: True} if flag else {}
    problems = inspect_map(linkmap.read_text(), **flags) + inspect_disassembly(dis.stdout, **flags)
    problems += inspect_image(flat.read_bytes(), **{k: True for k in ('t1v3', 't1t', 't2', 's2', 't4r', 't3r') if flag == k})
    return hashlib.sha256(flat.read_bytes()).hexdigest(), problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--clang', default='clang')
    parser.add_argument('--ld-lld', default='ld.lld')
    parser.add_argument('--objcopy', default='llvm-objcopy')
    parser.add_argument('--objdump', default='llvm-objdump')
    parser.add_argument('--only', nargs='*', choices=sorted(IMAGES))
    args = parser.parse_args(argv)
    version = subprocess.run([args.clang, '--version'], check=True, capture_output=True, text=True).stdout.splitlines()[
        0
    ]
    print(f'clang: {version}')
    failed = 0
    with tempfile.TemporaryDirectory() as scratch:
        for name in args.only or IMAGES:
            digest, problems = build(name, args, Path(scratch))
            print(f'{name} {digest} {"inspection passed" if not problems else "INSPECTION FAILED"}')
            for problem in problems:
                print(f'  {problem}', file=sys.stderr)
            failed += bool(problems)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
