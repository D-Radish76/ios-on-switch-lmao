"""The commands of a U-Boot diagnostic script, one token list each, and the commands no diagnostic may use."""

import re

STRUCTURE = {'if', 'then', 'else', 'elif', 'fi', 'for', 'do', 'done', 'while', 'in'}
FORBIDDEN = (
    *('bootm', 'bootz', 'booti', 'loados', 'saveenv', 'go', 'run', 'source', 'load', 'fatload', 'cp', 'cp.b'),
    *('mw', 'md', 'md.l', 'mm', 'nm', 'cmp', 'crc32', 'env', 'imxtract', 'dtimg'),
)


def commands(script: str) -> list[list[str]]:
    found = []
    for raw in script.splitlines():
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        if line.startswith('for '):
            line = 'for'
        for part in re.split(r';\s*|\s+then$|\s+do$', line):
            tokens = part.split()
            while tokens and tokens[0] in STRUCTURE:
                tokens = tokens[1:]
            if tokens:
                found.append(tokens)
    return found
