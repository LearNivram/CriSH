# How it fails

An error message is a promise about what happened. These are the ones CriSH
produces and what each one means.

## Shape

```
crish: FILE: line N: message
```

The file and line appear when a script is running. Interactively, only
`crish: message`. Utilities report as themselves:

```
sed: -e expression #1: unterminated `s' command
grep: nope.txt: No such file or directory
```

## Exit statuses

| | |
| --- | --- |
| `0` | success |
| `1` | the command failed, or `test` was false |
| `2` | a syntax error, or a usage error in a builtin |
| `124` | `timeout` fired |
| `125` | `timeout` itself was used wrongly |
| `126` | found, but not executable |
| `127` | command not found |
| `128 + N` | killed by signal N |

`$?` follows the last command; in a pipeline it is the last stage unless
`set -o pipefail`.

## Messages you are likely to see

**`crish: foo: command not found`** — nothing named `foo` is a function, a
builtin, a built-in utility or on `PATH`. `type foo` and `echo "$PATH"`.

**`crish: foo: unbound variable`** — `set -u` is on and `$foo` was never set.
`${foo:-}` if empty is acceptable.

**`crish: line 3: syntax error: expected 'fi'`** — a construct was left open.
CriSH names the construct it was waiting for rather than pointing at the token
that finally confused it.

**`crish: unexpected EOF while looking for matching '"'`** — an unclosed quote.
Interactively this is not an error: CriSH keeps reading and shows `PS2`.

**`crish: bad substitution: ${...}`** — a `${ }` form CriSH does not know.
Compare against [bash compatibility](bash.md).

**`crish: /path: Permission denied`** — a redirection could not open its
target. The command does not run, and the status is 1.

**`sed: -i is a GNU option here; the suffix is attached`** — you wrote
`sed -i '' 's/a/b/'` for BSD sed. In CriSH, and on Linux, that empty string is
the script. Drop it.

**`grep: (...): unmatched (`** — the regex flavour does not match the flag.
`grep -E '(a|b)'`, `grep '\(a\|b\)'`, or `grep -P`.

**`awk: syntax error at line N`** — the awk program, not the shell script.
Line N counts inside the awk program.

**`crish: %s: division by 0`** — arithmetic. The expression is echoed back so
you can see what it expanded to.

## Getting more out of a failure

```sh
set -x                  # trace every command with its expansions, prefixed by PS4
PS4='+ ${LINENO}: '     # put the line number in the trace
set -e                  # stop at the first failure
set -o pipefail         # a failing stage makes the whole pipeline fail
set -u                  # unset variables are an error
crish -n script.sh      # parse only: check the syntax without running anything
```

`set -euo pipefail` at the top of a script is as good an idea here as anywhere.

## When CriSH itself is wrong

If a script behaves differently under `crish` than under `bash` on Linux, that
is a bug, not a documented limitation, unless the behaviour is listed under
**Not implemented** in [bash compatibility](bash.md) or
[GNU compatibility](gnu.md). The most useful bug report is a case file:

```sh
# tests/cases/99-mine.crsh
suite mine
check the-thing "$(printf 'a b\n' | awk '{print $2}')" b
```

It has to fail under `crish` and pass under `bash` with GNU tools. That makes
it reproducible in CI on the first run.
