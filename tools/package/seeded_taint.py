"""Which actions of the seeded package hold the seed, found by following it through the action graph.

The roots are the actions that read the seed file. Every action that reads an output of a tainted action is tainted
too. Each of them must carry no-cache and no-remote, and the actions the package cannot do without must be among them.
The text format of `bazel aquery` is read here; when it does not look as expected the check refuses.

Blind spot of the walk: an action that reads a directory output (a tree artifact) may not list it as an input file, so
the walk can miss it. The 128 KernelCompress actions are such: each reads one directory the layout step wrote, and one of
them holds the seed. They are checked by their mnemonic instead (FIXED_SETS), by count and by tag, and the walk is kept.
"""

import re
import subprocess
from dataclasses import dataclass

import erista_seed

SEED_SOURCE = 'local/erista-seed.hex'
TAGS = ('no-cache', 'no-remote')
# Mnemonics that must turn up tainted: the check found the graph it was meant to look at.
REQUIRED = ('BootAdt', 'KernelLayout', 'KernelWindow', 'BootScript', 'BootBl33', 'BootPackage')
# Actions that run no process, so no cache stores their result and no tag applies.
INTERNAL = ('Symlink', 'SourceSymlinkManifest', 'SymlinkTree', 'FileWrite', 'ParameterFileWrite', 'TemplateExpand')
MAX_ROUNDS = 25
# Actions found by mnemonic, whatever the walk reports: the exact count is the number of window shards in boot/defs.bzl.
FIXED_SETS = {'KernelCompress': 128}


class TaintError(Exception):
    pass


@dataclass(frozen=True)
class Action:
    mnemonic: str
    inputs: tuple[str, ...]
    outputs: tuple[str, ...]
    info: str


def parse(text: str) -> list[Action]:
    actions = []
    for block in re.split(r'(?m)^action ', text)[1:]:
        fields = {}
        for line in block.splitlines():
            key, _, value = line.strip().partition(': ')
            if key in ('Mnemonic', 'Inputs', 'Outputs', 'ExecutionInfo') and key not in fields:
                fields[key] = value
        if 'Mnemonic' not in fields or 'Outputs' not in fields:
            raise TaintError('an aquery action block has no Mnemonic or Outputs line')
        actions.append(
            Action(
                fields['Mnemonic'],
                paths(fields.get('Inputs', '')),
                paths(fields['Outputs']),
                fields.get('ExecutionInfo', ''),
            )
        )
    return actions


def paths(text: str) -> tuple[str, ...]:
    return tuple(p.strip() for p in text.strip().strip('[]').split(',') if p.strip())


def pattern(names) -> str:
    """A regular expression for these paths. A character that is special anywhere is a wildcard: it can only widen the match."""
    return '|'.join(re.sub(r'[^A-Za-z0-9_/-]', '.', name) for name in sorted(names))


def run_query(expression, extra, run) -> list[Action]:
    command = [
        'bazel',
        'aquery',
        *erista_seed.BUILD_FLAGS,
        *extra,
        *erista_seed.CACHE_FLAGS,
        '--//boot:erista_boot_run=false',
        '--output=text',
        expression,
    ]
    done = run(command, capture_output=True, text=True, check=False)
    if done.returncode != 0:
        raise TaintError('could not follow the seed through the action graph')
    return parse(done.stdout)


def query(names, extra, run) -> list[Action]:
    return run_query(f'inputs("{pattern(names)}", deps({erista_seed.TARGET}))', extra, run)


def fixed_set(mnemonic, extra, run) -> list[Action]:
    """Every action of a mnemonic in the package's graph: exactly the expected number, none twice, each fully tagged."""
    found = run_query(f'mnemonic("{mnemonic}", deps({erista_seed.TARGET}))', extra, run)
    found = [a for a in found if a.mnemonic == mnemonic]
    if len({a.outputs for a in found}) != len(found):
        raise TaintError(f'{mnemonic} was listed twice for one output: the count cannot be trusted')
    if len(found) != FIXED_SETS[mnemonic]:
        raise TaintError(f'{len(found)} {mnemonic} actions found, {FIXED_SETS[mnemonic]} expected')
    return found


def tainted(extra=(), run=subprocess.run) -> list[Action]:
    """Every action that reads the seed, or an output of one that does, repeated until nothing new turns up."""
    frontier, found = {SEED_SOURCE}, {}
    for _ in range(MAX_ROUNDS):
        fresh = [a for a in query(frontier, extra, run) if a.outputs not in found]
        if not fresh:
            return list(found.values())
        for action in fresh:
            found[action.outputs] = action
        frontier = {out for action in fresh for out in action.outputs}
    raise TaintError(f'the seed is still spreading after {MAX_ROUNDS} rounds')


def has(info: str, tag: str) -> bool:
    return re.search(rf'(?<![\w-]){re.escape(tag)}(?![\w-])', info) is not None


def check_tags(action) -> None:
    if action.mnemonic in INTERNAL:
        return
    for tag in TAGS:
        if not has(action.info, tag):
            raise TaintError(f'{action.mnemonic} holds the seed and lacks {tag}')


def seed_actions_are_private(extra=(), run=subprocess.run) -> dict[str, int]:
    """Mnemonic to count of the actions that hold the seed, from the walk and from FIXED_SETS (each action once, by its
    outputs). Raises unless a reader of the seed exists, every action that runs a process has both tags, REQUIRED turn up
    and each fixed set has its exact count. An unknown mnemonic is never exempt."""
    actions = tainted(extra, run)
    roots = [a for a in actions if SEED_SOURCE in ' '.join(a.inputs)]
    if not roots:
        raise TaintError('no action reads the seed file: the graph was not the seeded one')
    every = {a.outputs: a for a in actions}
    for mnemonic in FIXED_SETS:
        every.update({a.outputs: a for a in fixed_set(mnemonic, extra, run)})
    counts: dict[str, int] = {}
    for action in every.values():
        counts[action.mnemonic] = counts.get(action.mnemonic, 0) + 1
        check_tags(action)
    missing = [m for m in REQUIRED if m not in counts]
    if missing:
        raise TaintError(f'{missing[0]} was not found among the actions that hold the seed')
    return dict(sorted(counts.items()))
