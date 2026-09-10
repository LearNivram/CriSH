# Contributing

Issues and pull requests are welcome. This file says what a change needs to
bring with it so that review is short.

## Getting set up

```sh
git clone https://github.com/LearNivram/CriSH
cd CriSH
make test
```

That is the whole toolchain: `cc` and `make`. If `make test` is green you are
ready.

To run the differential comparison locally you also want the GNU tools:

```sh
brew install bash coreutils gnu-sed gawk grep
PATH="$(brew --prefix)/opt/coreutils/libexec/gnubin:$PATH" \
  "$(brew --prefix)/bin/bash" tests/run.crsh
```

Both runs must be green.

## Branches

```
feat/associative-array-slicing
fix/heredoc-after-heredoc
perf/glob-stat-cache
docs/gnu-sed-limits
test/awk-getline
ci/macos-13-runner
refactor/split-expand
chore/bump-version
```

A prefix from that list, then kebab case.

## Commits

One logical change per commit, present tense, no trailing period:

```
fix: keep stdin's FILE in step with its descriptor

A built-in utility runs in the shell's own process, so the EOF flag on
the stdin FILE outlived a redirection of fd 0 and the second heredoc in
a script read nothing.
```

The subject line says what changed; the body says why, if that is not
obvious. `Fixes #12` on its own line when it closes an issue.

## What a change needs

| Kind of change | Needs |
| --- | --- |
| A bug fix | a check in `tests/cases/` that fails before and passes after |
| A new shell feature | checks, and a line in `docs/bash.md` |
| A new built-in utility | checks, and a **Works** / **Not implemented** section in `docs/gnu.md` |
| A new option on an existing utility | checks, and the option listed in `docs/gnu.md` |
| A performance change | a number from `tools/bench.sh` before and after |
| Documentation | nothing else |

The **Not implemented** half is not optional. This project is worth using
because its compatibility page is true; a change that quietly leaves a gap
undocumented is worse than no change.

## Style

The code follows the Linux kernel style, because it is unambiguous and every C
programmer can read it:

- tabs for indentation, eight columns wide
- 90 columns is the soft limit
- `snake_case` for functions and variables, `CamelCase` for types
- declarations at the top of a block
- braces on the same line, except for functions
- no `typedef`ed pointers

`make fmt` runs `clang-format` if you have it. Warnings are errors in CI:

```sh
CFLAGS='-O2 -Werror' make
```

and one sanitiser run before you push is a good habit:

```sh
make clean && CFLAGS='-O1 -g -fsanitize=address,undefined' make && \
  ASAN_OPTIONS=detect_leaks=0 ./build/crish tests/run.crsh
```

## Review

Pull requests need CI green on `macos-14`, `macos-13` and the Linux
differential job. Small pull requests get read quickly; a 2,000 line one will
sit. If you are planning something large, open an issue first and say what you
have in mind.

## Releases

Maintainers only:

1. Bump `CRISH_VERSION` in `src/shell.h`.
2. Update `Formula/crish.rb` and the tap.
3. `git tag -a v0.2.0 -m 'v0.2.0'` and push the tag.
4. `.github/workflows/release.yml` builds the universal binary, writes
   `checksums.txt` and publishes the release.
5. Check that `crish update --check` sees it.

## Licence

By contributing you agree that your work is licensed under the GPL-3.0-or-later,
like the rest of the project.
