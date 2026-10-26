#!/usr/bin/env python3
"""Evaluate build-xnu-4k-baseline.sh's configuration guards against a .config, without a macOS host.

Why this exists
---------------
The XNU build only runs on macOS, so a rejected configuration is normally discovered after a
thirty-minute runner dispatch. Every configuration refusal in build-xnu-4k-baseline.sh is an
`if <condition>; then echo '<reason>' >&2; exit 1; fi`, so the conditions can be extracted and evaluated
here in about a second. That turns a wasted dispatch into an instant, named failure.

This is a pre-flight, not a substitute for the build: it checks the refusal conditions only, not whether
the selected drivers compile or whether the resulting kernel runs.

What it reads
-------------
The guards are more varied than they look. All of these shapes occur in the script:

    if [ "${XNU_NATIVE_AES:-0}" = 1 ]; then
      if [ "$CRYPTO_BOOTSTRAP_MODE" != prelinked ] || [ "${XNU_SOURCE_CORECRYPTO14:-0}" != 1 ]; then
    if [ "${XNU_NATIVE_UFS:-0}" != 1 ] || [ "${XNU_OWNER_ROOT_AUDIT:-0}" != 1 ] || \
       [ ... ]; then
    if [ "$MOCKFS_MODE" = 1 ] && { [ "${XNU_ARMCPU:-0}" != 1 ] || [ ... ]; }; then

so the parser joins backslash continuations, then evaluates the whole `if` condition. It resolves
`${XNU_X:-default}` and `$LOCAL_VAR` (following the script's own `VAR=...` assignments), and supports
`[ ]` tests, `||`, `&&` and `{ ...; }` grouping.

Conditions that touch the environment rather than the configuration -- `-f`, `-d`, `$(...)`, git, cat,
uname -- are classified as environment checks and skipped, because they cannot be decided from a .config.
Anything else it cannot evaluate is reported as UNKNOWN, never as a pass.

Exit status: 0 all guards satisfied, 1 at least one guard refuses this configuration, 3 undecidable
guards only, 2 usage.
"""

import re
import sys
from pathlib import Path


# --- .config ---------------------------------------------------------------------------------------

def read_config(path):
    """Parse a kernel .config into {NAME: value}: y/n become 1/0, quoted strings lose their quotes.

    The defconfig spells options CONFIG_XNU_FOO while build-xnu-4k-baseline.sh tests XNU_FOO, so the
    CONFIG_ prefix is stripped here. Without that every lookup misses and every guard looks triggered.
    """
    values = {}
    for line in Path(path).read_text(encoding='utf-8').splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        name, sep, raw = line.partition('=')
        if not sep:
            continue
        name = name.strip()
        if name.startswith('CONFIG_'):
            name = name[len('CONFIG_'):]
        raw = raw.strip()
        if len(raw) >= 2 and raw[0] == raw[-1] == '"':
            values[name] = raw[1:-1]
        elif raw in ('y', 'n'):
            values[name] = '1' if raw == 'y' else '0'
        else:
            values[name] = raw
    return values


# --- script model ----------------------------------------------------------------------------------

ASSIGN = re.compile(r'^\s*([A-Z_][A-Z0-9_]*)=("[^"]*"|\'[^\']*\'|\S*)\s*$')
IF_HEAD = re.compile(r'^\s*if\s+(.*?);\s*then\s*$')
# Things that make a condition an environment probe rather than a configuration test.
ENV_MARKERS = ('$(', 'git ', '-f ', '-d ', '-e ', 'cat ', 'uname', 'command -v', 'test -')


def logical_lines(raw_lines):
    """Join backslash continuations so one entry is one shell statement."""
    out, buffer = [], ''
    for line in raw_lines:
        stripped = line.rstrip()
        if stripped.endswith('\\'):
            buffer += stripped[:-1] + ' '
            continue
        out.append(buffer + stripped)
        buffer = ''
    if buffer:
        out.append(buffer)
    return out


def script_assignments(lines):
    """Collect `VAR=value` assignments the conditions refer to, keeping the right-hand side verbatim.

    The right-hand side may itself be a parameter expansion, including a nested one such as
    `NATIVE_ARCH_TIMER=${XNU_NATIVE_ARCH_TIMER:-${XNU_ARMCPU:-0}}`, so it is stored raw and expanded
    later by expand_text() rather than pattern-matched here.
    """
    found = {}
    for line in lines:
        m = ASSIGN.match(line)
        if not m:
            continue
        name, raw = m.group(1), m.group(2)
        if raw[:1] in '"\'':
            raw = raw[1:-1]
        found[name] = raw
    return found


# ${NAME} or ${NAME:-default}, where the default may itself contain one level of braces.
EXPAND = re.compile(r'\$\{([A-Z_][A-Z0-9_]*)(?::-((?:[^{}]|\{[^{}]*\})*))?\}')


def expand_text(text, assigns, config, depth=0):
    """Expand ${...} references in `text`. Returns None when a reference cannot be resolved."""
    if depth > 8:
        return None
    failed = False

    def repl(match):
        nonlocal failed
        name, default = match.group(1), match.group(2)
        value = config.get(name)
        if value not in (None, ''):
            return value
        if default is not None:
            expanded = expand_text(default, assigns, config, depth + 1)
            if expanded is None:
                failed = True
                return ''
            return expanded
        # ${NAME} with no default: fall back to the script's own assignment for NAME.
        if name in assigns:
            expanded = expand_text(assigns[name], assigns, config, depth + 1)
            if expanded is None:
                failed = True
                return ''
            return expanded
        failed = True
        return ''

    out = EXPAND.sub(repl, text)
    return None if failed else out


def resolve(var, assigns, config):
    """Resolve a shell variable to a string, following ${...} indirection. None if unknown."""
    if var.startswith('XNU_'):
        return config.get(var)
    if var not in assigns:
        return None
    return expand_text(assigns[var], assigns, config)


# --- condition evaluation --------------------------------------------------------------------------

def eval_test(body, assigns, config):
    """Evaluate the inside of one `[ ... ]`. True/False/None."""
    body = body.strip()
    if not body:
        return None
    negate = False
    if body.startswith('!'):
        negate = True
        body = body[1:].strip()

    m = re.fullmatch(r'-z\s+"?\$\{?([A-Z_][A-Z0-9_]*)(?::-([^}]*))?\}?"?', body)
    if m:
        value = resolve(m.group(1), assigns, config)
        if value in (None, '') and m.group(2) is not None:
            value = m.group(2)
        result = None if value is None else (value == '')
        return None if result is None else (not result if negate else result)

    m = re.fullmatch(r'-n\s+"?\$\{?([A-Z_][A-Z0-9_]*)(?::-([^}]*))?\}?"?', body)
    if m:
        value = resolve(m.group(1), assigns, config)
        if value in (None, '') and m.group(2) is not None:
            value = m.group(2)
        result = None if value is None else (value != '')
        return None if result is None else (not result if negate else result)

    # LEFT op RIGHT where either side is $VAR / ${XNU_VAR:-default} / a bare word.
    m = re.fullmatch(r'(.+?)\s*(!?=)\s*(.+)', body)
    if not m:
        return None
    left_raw, op, right_raw = m.group(1).strip(), m.group(2), m.group(3).strip()

    def operand(text):
        text = text.strip().strip('"')
        ref = re.fullmatch(r'\$\{?([A-Z_][A-Z0-9_]*)(?::-([^}]*))?\}?', text)
        if ref:
            value = resolve(ref.group(1), assigns, config)
            if value in (None, '') and ref.group(2) is not None:
                return ref.group(2)
            return value
        return text

    left, right = operand(left_raw), operand(right_raw)
    if left is None or right is None:
        return None
    result = (left == right) if op == '=' else (left != right)
    return not result if negate else result


def strip_outer_brace(text):
    """Remove one layer of `{ ...; }` if the braces enclose the whole expression (balanced)."""
    text = text.strip()
    if not (text.startswith('{') and text.endswith('}')):
        return text
    depth = 0
    for i, ch in enumerate(text):
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0 and i != len(text) - 1:
                return text      # the closing brace is not the last character: not a single group
    return text[1:-1].strip().rstrip(';').strip()


def split_top(text, op):
    """Split on `op` at nesting depth 0, tracking [], {}, and () so grouped sub-expressions stay whole."""
    parts, current = [], ''
    depth = 0
    i = 0
    while i < len(text):
        ch = text[i]
        if ch in '[{(':
            depth += 1
        elif ch in ']})':
            depth -= 1
        if depth == 0 and text[i:i + len(op)] == op:
            parts.append(current)
            current = ''
            i += len(op)
            continue
        current += ch
        i += 1
    parts.append(current)
    return parts


def eval_expr(expr, assigns, config):
    """Evaluate `[..] || [..] && [..]` with `{ ...; }` grouping. True/False/None."""
    expr = expr.strip()
    if not expr:
        return None

    expr = strip_outer_brace(expr)
    if not expr:
        return None

    # || binds looser than &&.
    ors = split_top(expr, '||')
    if len(ors) > 1:
        results = [eval_expr(p, assigns, config) for p in ors]
        if any(r is True for r in results):
            return True
        if any(r is None for r in results):
            return None
        return False

    ands = split_top(expr, '&&')
    if len(ands) > 1:
        results = [eval_expr(p, assigns, config) for p in ands]
        if any(r is False for r in results):
            return False
        if any(r is None for r in results):
            return None
        return True

    m = re.fullmatch(r'\[\s*(.*?)\s*\]', expr, re.S)
    if m:
        return eval_test(m.group(1), assigns, config)

    return None


def is_config_condition(cond):
    """False when the condition depends on the host (files, git, tools) rather than the configuration."""
    return not any(marker in cond for marker in ENV_MARKERS)


def _group(condition):
    """Wrap a condition so it can be joined with && without changing its own precedence."""
    body = condition if condition.startswith('[') else f'[ {condition} ]'
    if '||' in body:
        return '{ ' + body + '; }'
    return body


def find_guards(lines):
    """Yield (condition, message) for every refusal, with enclosing `if` conditions folded in.

    A refusal nested inside another `if` only applies when that outer condition holds too -- the
    boot-progress guard, for example, lives inside `if [ "${XNU_BOOT_PROGRESS:-0}" = 1 ]`. Without
    folding, a disabled outer option would make the inner refusal look triggered.

    Enclosing conditions are joined with `&&`; any that contain `||` are brace-grouped first, because
    `||` binds looser than `&&` and a bare join would silently change the meaning.
    """
    guards = []
    stack = []          # conditions of the `if`s we are currently inside
    pending = None      # most recent echo, the refusal message
    for line in lines:
        stripped = line.strip()
        head = IF_HEAD.match(line)
        if head:
            stack.append(head.group(1))
            continue
        if stripped == 'fi':
            if stack:
                stack.pop()
            continue
        if stripped.startswith('echo '):
            pending = stripped
            continue
        if stripped == 'exit 1' and stack:
            guards.append((' && '.join(_group(c) for c in stack), pending))
            pending = None
    return guards


def main():
    if len(sys.argv) != 3:
        print('usage: check-build-guards.py BUILD_SCRIPT CONFIG', file=sys.stderr)
        return 2
    script, config_path = Path(sys.argv[1]), Path(sys.argv[2])
    lines = logical_lines(script.read_text(encoding='utf-8').splitlines())
    config = read_config(config_path)
    assigns = script_assignments(lines)

    guards = find_guards(lines)
    triggered, unknown, satisfied, skipped = [], [], 0, 0
    for condition, message in guards:
        if not is_config_condition(condition):
            skipped += 1
            continue
        result = eval_expr(condition, assigns, config)
        if result is True:
            triggered.append((condition, message))
        elif result is False:
            satisfied += 1
        else:
            unknown.append((condition, message))

    print(f'{len(guards)} refusal guards in the script; {skipped} depend on the host and were skipped')
    print(f'{satisfied} configuration guard(s) evaluated and not triggered')
    if triggered:
        print(f'\nFAIL: {len(triggered)} guard(s) would exit 1 with this configuration:')
        for condition, message in triggered:
            print(f'  when: {condition}')
            if message:
                print(f'  then: {message}')
    if unknown:
        print(f'\nUNKNOWN: {len(unknown)} guard(s) could not be decided from a .config:')
        for condition, message in unknown:
            print(f'  when: {condition}')
            if message:
                print(f'  then: {message}')
    if not triggered and not unknown:
        print('\nno configuration guard refuses this .config')
        return 0
    return 1 if triggered else 3


if __name__ == '__main__':
    sys.exit(main())
