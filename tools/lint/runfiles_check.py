r"""Run py_tests in a directory that holds only what their BUILD rule declares.

    runfiles_check.py [TEST_NAME ...]        (default: every py_test of tools/package/test)

Bazel gives a test its `srcs`, `data` and transitive `deps` and nothing else, and refuses a source file another
package has not exported. A direct unittest run sees the whole tree and misses both. This copies the declared files
into a temporary directory (same layout), puts the `imports` of the deps on PYTHONPATH, checks that every file label
is exported or in the test's own package, and runs the test there with the working directory at its root. It is a
stand-in for Bazel: filegroups, globs, exports_files, py_library and py_binary are understood, nothing else is.
Stdlib only.
"""

import ast
import fnmatch
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class Package:
    def __init__(self, name: str) -> None:
        self.name = name
        self.rules: dict[str, dict] = {}
        self.exports: list[str] = []
        self.public = False
        path = ROOT / name / 'BUILD.bazel'
        if path.exists():
            self.parse(path.read_text())

    def literal(self, node: ast.AST):  # noqa: ANN201
        if isinstance(node, ast.Call) and getattr(node.func, 'id', '') == 'glob':
            patterns = self.literal(node.args[0])
            exclude = next((self.literal(k.value) for k in node.keywords if k.arg == 'exclude'), [])
            base = ROOT / self.name
            found = sorted(
                str(p.relative_to(base))
                for p in base.rglob('*')
                if p.is_file() and not any(part.startswith('.') for part in p.relative_to(base).parts[:-1])
            )
            return [
                f
                for f in found
                if any(fnmatch.fnmatch(f, g) or _glob(f, g) for g in patterns) and not any(_glob(f, e) for e in exclude)
            ]
        if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
            left, right = self.literal(node.left), self.literal(node.right)
            return left + right if isinstance(left, list) and isinstance(right, list) else None
        try:
            return ast.literal_eval(node)
        except ValueError:
            return None

    def parse(self, text: str) -> None:
        for node in ast.parse(text).body:
            if not (isinstance(node, ast.Expr) and isinstance(node.value, ast.Call)):
                continue
            call = node.value
            func = getattr(call.func, 'id', '')
            kwargs = {k.arg: self.literal(k.value) for k in call.keywords if k.arg}
            if func == 'package':
                self.public = '//visibility:public' in (kwargs.get('default_visibility') or [])
            elif func == 'exports_files':
                self.exports += self.literal(call.args[0]) or []
            elif kwargs.get('name'):
                self.rules[kwargs['name']] = {'kind': func, **kwargs}


def _glob(path: str, pattern: str) -> bool:
    if '**' not in pattern:
        return fnmatch.fnmatch(path, pattern)
    head, _, tail = pattern.partition('**')
    return path.startswith(head) and (not tail.strip('/*') or fnmatch.fnmatch(path.rsplit('/', 1)[-1], tail.strip('/')))


PACKAGES: dict[str, Package] = {}


def package(name: str) -> Package:
    return PACKAGES.setdefault(name, Package(name))


def split(label: str, here: str) -> tuple[str, str]:
    if label.startswith('//'):
        pkg, _, name = label[2:].partition(':')
        return pkg, name
    return here, label.removeprefix(':')


class Collector:
    def __init__(self) -> None:
        self.files: set[Path] = set()
        self.imports: set[str] = set()
        self.problems: list[str] = []

    def add(self, label: str, here: str, owner: str) -> None:
        pkg_name, name = split(label, here)
        pkg = package(pkg_name)
        if name in pkg.rules:
            rule = pkg.rules[name]
            if pkg_name != here and not pkg.public:
                self.problems.append(f'{owner}: {label} is not visible from {here}')
            self.rule(pkg_name, rule)
            return
        path = ROOT / pkg_name / name
        if not path.is_file():
            self.problems.append(f'{owner}: {label} is neither a rule nor a file')
        elif pkg_name != here and not any(_glob(name, e) for e in pkg.exports):
            self.problems.append(f'{owner}: {label} is not exported by //{pkg_name} (exports_files)')
        else:
            self.files.add(path.relative_to(ROOT))

    def rule(self, pkg_name: str, rule: dict) -> None:
        for key in ('srcs', 'hdrs', 'data', 'deps'):
            resolved = ['//{}:{}'.format(*split(label, pkg_name)) for label in rule.get(key) or []]
            for label in sorted({label for label in resolved if resolved.count(label) > 1}):
                self.problems.append(f'{rule["kind"]} {rule["name"]} ({key}): {label} is listed more than once')
        for key in ('srcs', 'data', 'deps'):
            for label in rule.get(key) or []:
                self.add(label, pkg_name, f'{rule["kind"]} {rule["name"]} ({key})')
        for entry in rule.get('imports') or []:
            self.imports.add(str((Path(pkg_name) / entry).as_posix()))


def run(name: str) -> int:
    pkg_name = 'tools/package/test'
    rule = package(pkg_name).rules.get(name)
    if not rule or rule['kind'] != 'py_test':
        print(f'{name}: no py_test of that name in {pkg_name}')
        return 2
    collector = Collector()
    collector.rule(pkg_name, rule)
    if collector.problems:
        print(f'{name}: FAIL, not buildable under Bazel')
        print('\n'.join(f'   {p}' for p in sorted(set(collector.problems))))
        return 1
    script = Path(pkg_name) / rule['srcs'][0]
    with tempfile.TemporaryDirectory() as scratch:
        for rel in collector.files:
            (Path(scratch) / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / rel, Path(scratch) / rel)
        env = {k: v for k, v in os.environ.items() if k != 'PYTHONPATH'}
        env['PYTHONPATH'] = os.pathsep.join([scratch, *(f'{scratch}/{i}' for i in sorted(collector.imports))])
        result = subprocess.run(
            [sys.executable, str(script)], cwd=scratch, env=env, capture_output=True, text=True, check=False
        )
        tail = (result.stdout + result.stderr).strip().splitlines()
        print(f'{name}: {"PASS" if result.returncode == 0 else "FAIL"} ({len(collector.files)} files in the sandbox)')
        if result.returncode:
            print('\n'.join(f'   {line}' for line in tail[-25:]))
    return result.returncode


def main() -> int:
    names = sys.argv[1:] or [n for n, r in package('tools/package/test').rules.items() if r['kind'] == 'py_test']
    return max((run(n) for n in names), default=0)


if __name__ == '__main__':
    sys.exit(main())
