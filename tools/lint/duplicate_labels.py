"""Fail on a label listed twice in one label attribute of a BUILD file, which Bazel refuses at analysis time.

    duplicate_labels.py [BUILD_FILE ...]    (default: every tracked BUILD.bazel and BUILD)

A label is the same label with or without its package prefix, so ":a" and "//pkg:a" in //pkg are one. Only
attributes whose values are label lists are read; flag lists such as copts repeat legitimately.
"""

import ast
import subprocess
import sys
from pathlib import Path

LABEL_ATTRS = ('srcs', 'hdrs', 'textual_hdrs', 'data', 'deps', 'tools', 'outs')


def normalise(label: str, package: str) -> str:
    if label.startswith('//'):
        pkg, _, name = label[2:].partition(':')
        return f'//{pkg}:{name or pkg.rsplit("/", 1)[-1]}'
    if label.startswith(':'):
        return f'//{package}:{label[1:]}'
    return f'//{package}:{label}'


def find(text: str, package: str) -> list[str]:
    problems = []
    for node in ast.walk(ast.parse(text)):
        if not isinstance(node, ast.Call):
            continue
        name = next((k.value.value for k in node.keywords if k.arg == 'name' and isinstance(k.value, ast.Constant)), '?')
        for keyword in node.keywords:
            if keyword.arg not in LABEL_ATTRS or not isinstance(keyword.value, ast.List):
                continue
            labels = [normalise(e.value, package) for e in keyword.value.elts if isinstance(e, ast.Constant) and isinstance(e.value, str)]
            problems += [
                f'{package}: {name} ({keyword.arg}): {label} is listed more than once'
                for label in sorted({label for label in labels if labels.count(label) > 1})
            ]
    return problems


def tracked() -> list[Path]:
    out = subprocess.run(['git', 'ls-files', '*BUILD.bazel', '*/BUILD', 'BUILD'], capture_output=True, text=True, check=True)
    return [Path(line) for line in out.stdout.splitlines()]


def main(argv: list[str]) -> int:
    problems = []
    for path in [Path(a) for a in argv] or tracked():
        problems += find(path.read_text(), path.parent.as_posix().removeprefix('.'))
    print('\n'.join(problems) if problems else 'no label is listed twice')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
