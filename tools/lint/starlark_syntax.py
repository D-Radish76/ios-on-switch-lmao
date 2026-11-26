"""Rejects Python that parses but is not Starlark in the repository's .bzl and BUILD files."""

import ast
import subprocess
import sys

_BANNED = {
    ast.While: "while loops",
    ast.Try: "try",
    ast.ClassDef: "class",
    ast.Import: "import",
    ast.ImportFrom: "import",
    ast.Yield: "yield",
    ast.YieldFrom: "yield",
    ast.Global: "global",
    ast.Nonlocal: "nonlocal",
    ast.With: "with",
    ast.AsyncFunctionDef: "async",
    ast.JoinedStr: "f-strings",
    ast.Set: "set literals",
    ast.SetComp: "set comprehensions",
    ast.NamedExpr: "the walrus operator",
    ast.Raise: "raise (use fail)",
    ast.Assert: "assert",
}


def check_source(source, name="<source>"):
    problems = []
    try:
        tree = ast.parse(source, name)
    except SyntaxError as error:
        return ["%s:%s: %s" % (name, error.lineno, error.msg)]
    for node in ast.walk(tree):
        line = getattr(node, "lineno", 0)
        for kind, what in _BANNED.items():
            if isinstance(node, kind):
                problems.append("%s:%d: %s" % (name, line, what))
        if isinstance(node, ast.Compare):
            if len(node.ops) > 1:
                problems.append("%s:%d: chained comparison, use `and` or `or` between two comparisons" % (name, line))
            if any(isinstance(op, (ast.Is, ast.IsNot)) for op in node.ops):
                problems.append("%s:%d: `is` comparison" % (name, line))
        if isinstance(node, ast.BinOp) and isinstance(node.op, (ast.Pow, ast.MatMult)):
            problems.append("%s:%d: operator not in Starlark" % (name, line))
        if isinstance(node, ast.Constant) and isinstance(node.value, complex):
            problems.append("%s:%d: literal type not in Starlark" % (name, line))
    for node in tree.body:
        if isinstance(node, (ast.For, ast.If)):
            problems.append("%s:%d: top-level statement `%s`" % (name, node.lineno, type(node).__name__.lower()))
    return problems


def tracked_files():
    names = subprocess.check_output(["git", "ls-files", "*.bzl", "*BUILD.bazel", "BUILD", "*/BUILD"], text=True)
    return [name for name in names.splitlines() if name]


def main(paths):
    problems = []
    for path in paths or tracked_files():
        with open(path, encoding="utf-8") as handle:
            problems += check_source(handle.read(), path)
    for problem in problems:
        print(problem)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
