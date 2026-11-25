"""A syntax check for the U-Boot scripts this package writes, for what hush is known to choke on.

hush reads a script a command at a time and has no recovery from a bad quote: a single `'` in an unquoted echo made
the memory diagnostic's page 1 fail on the Erista with a syntax error, garbage and `SCRIPT FAILED`, and nothing after
it ran. Command allow lists do not see that, so this checks the text itself:

* every character is printable ASCII;
* an unquoted `echo` holds only letters, digits, spaces and `, . : = / _ [ ] @ + % - ( )` and `${name}` references;
* `echoe` takes one double quoted string of the same characters (the form the normal script already runs with);
* `${` always closes, and `if`/`for`/`while` are closed by `fi`/`done`, in order.

Parentheses are allowed because a line with them ran in the first diagnostic (as a parsed, not executed, line).
"""

import re

TEXT = r'(?:[A-Za-z0-9 ,.:=/_\[\]@+%()\-]|\$\{[A-Za-z_][A-Za-z0-9_]*\})*'
PLAIN_ECHO = re.compile(rf'^echo(?: {TEXT})?$')
QUOTED_ECHO = re.compile(rf'^echoe "{TEXT}"$')
OPENERS = {'if': 'fi', 'for': 'done', 'while': 'done'}
CLOSERS = {'fi', 'done'}
SEPARATORS = re.compile(r';\s*')


def lint_line(line: str) -> list[str]:
    text = line.strip()
    if not text or text.startswith('#'):
        return []
    if any(not (32 <= ord(c) < 127) for c in text):
        return [f'a character outside printable ASCII: {text!r}']
    if text.startswith('echo ') or text == 'echo':
        return [] if PLAIN_ECHO.match(text) else [f'echo text hush may not parse: {text}']
    if text.startswith('echoe '):
        return [] if QUOTED_ECHO.match(text) else [f'echoe is not one plain double quoted string: {text}']
    if not _braces_close(text):
        return [f'${{ is not closed: {text}']
    return [] if _quotes_close(text) else [f'a quote is not closed: {text}']


def _braces_close(text: str) -> bool:
    return all(text.find('}', m.end()) >= 0 for m in re.finditer(r'\$\{', text))


def _quotes_close(text: str) -> bool:
    quote = ''
    for char in text:
        if quote:
            if char == quote:
                quote = ''
        elif char in '\'"':
            quote = char
    return not quote


def lint_blocks(text: str) -> list[str]:
    problems = []
    stack: list[str] = []
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith(('#', 'echo ', 'echoe ')):
            continue
        for part in SEPARATORS.split(line):
            words = part.split()
            if not words:
                continue
            word = words[0]
            if word in OPENERS:
                stack.append(OPENERS[word])
            elif word in CLOSERS and (not stack or stack.pop() != word):
                problems.append(f'line {number}: {word} does not close the block that is open')
    problems += [f'a block is still open at the end, expected {closer}' for closer in reversed(stack)]
    return problems


def lint(text: str) -> list[str]:
    """Return what is wrong with a script (an empty list is clean). Placeholders such as @READS@ are plain text."""
    problems = []
    for number, line in enumerate(text.splitlines(), 1):
        problems += [f'line {number}: {problem}' for problem in lint_line(line)]
    return problems + lint_blocks(text)
