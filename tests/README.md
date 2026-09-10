# The test suite

```sh
make test                       # build, then run everything under CriSH
./build/crish tests/run.crsh    # the same thing
```

269 checks across ten case files. A failure prints the check name, what it got
and what it wanted, and the run exits non-zero.

## Why it is differential

The suite is written so that **the same files run under bash 5 with GNU
coreutils** and produce the same answers. That is the compatibility claim, made
mechanical:

```sh
# on a Mac with the GNU tools installed for comparison
brew install bash coreutils gnu-sed gawk grep
PATH="$(brew --prefix)/opt/coreutils/libexec/gnubin:$PATH" \
  "$(brew --prefix)/bin/bash" tests/run.crsh
```

CI does exactly this on `ubuntu-latest`, where bash 5 and GNU coreutils are the
system tools and no `PATH` games are needed. If a check answers differently
under CriSH than under real bash and real GNU, the build is red.

This is why the case files avoid anything CriSH-specific. A check that only
CriSH could pass would prove nothing.

## Layout

```
tests/
  run.crsh              the runner: sets LC_ALL=C, makes a scratch dir, sources each case
  lib.crsh              suite / check / ok / progress / summary
  cases/
    01-expansion.crsh   parameter, arithmetic, brace and command expansion
    02-arrays.crsh      indexed and associative arrays, mapfile
    03-control.crsh     loops, case, functions, recursion
    04-conditionals.crsh  test, [[ ]], =~ and BASH_REMATCH
    05-redirection.crsh   redirections, heredocs, pipes, process substitution
    06-gnu-text.crsh      grep, sed, awk
    07-gnu-tools.crsh     the rest of the built-in utilities
    08-shell-options.crsh globbing, set and shopt
    09-strings.crsh       printf, echo, read, quoting
    10-macos-gaps.crsh    only things a stock macOS gets wrong
  cases-crish/          CriSH-only; skipped when the suite runs under bash
    01-color.crsh         colour decisions, themes, the highlighter
    02-color-tools.crsh   grep --color and the built-in ls
```

`cases-crish/` exists so that the differential property stays exact: a check
in `cases/` must hold under bash 5 with GNU tools, and anything that could not
possibly hold there goes in `cases-crish/` instead.

`10-macos-gaps.crsh` is the file worth reading first: every check in it fails on
a stock Mac with `/bin/bash` and the BSD userland.

## Writing a check

```sh
suite my-area

check name-of-the-check  "$(the thing)"  "what it should be"
[[ some condition ]] && ok another-check yes
```

`check` compares two strings. `ok name yes` is shorthand for a condition that
should hold. `$(scratch)` gives a temporary directory that is removed when the
run finishes.

Rules:

1. It must pass under CriSH **and** under bash 5 with GNU tools.
2. No dependency on the machine's locale, timezone, hostname or user.
3. No network.
4. Times and dates go through `date -u -d @SECONDS`, never `date` alone.

## Reporting a bug as a test

The most useful bug report is a case file that fails here and passes under
bash. Drop it in `tests/cases/` and open a pull request; a red CI run on a new
test is a fine first commit.
