# Building

Everything you need is on a Mac already: `cc` (Xcode command line tools) and
`make`. There is no CMake, no configure step, no dependency to fetch. The
binary links `libSystem` and nothing else.

## This machine

```sh
make            # build/crish for the architecture you are on
make test       # build, then run the whole suite
make clean
```

## Universal

```sh
./build.sh      # arm64 and x86_64 in one binary
```

clang emits both slices from one invocation, so there is no `lipo` step. Check
with `lipo -archs build/crish`.

Override the pieces:

```sh
CC=clang CFLAGS='-O3 -march=native' make
ARCHS='arm64' ./build.sh
OUT=/tmp/crish ./build.sh
```

## Installing what you built

```sh
sudo make install               # /usr/local/bin/crish
PREFIX=~/.local make install    # ~/.local/bin/crish
sudo make uninstall
```

## While working on it

A debug build with the sanitisers, which is what CI runs on every push:

```sh
make clean
CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' make
./build/crish tests/run.crsh
```

ASan will report a leak summary on exit; CriSH does not free everything at
shutdown on purpose, so run with `ASAN_OPTIONS=detect_leaks=0` unless you are
specifically hunting leaks.

Warnings are errors in CI. Build with them locally before you push:

```sh
CFLAGS='-O2 -Werror' make
```

## The layout

```
src/
  main.c        startup, option parsing, the interactive loop
  util.c        buffers, string vectors, allocation
  parser.c      lexer and recursive descent parser
  expand.c      brace, tilde, parameter, command and arithmetic expansion
  arith.c       $(( ))
  glob.c        pattern matching and pathname expansion
  vars.c        variables, scopes, indexed and associative arrays
  exec.c        running the tree
  redir.c       redirections
  cond.c        test, [ ] and [[ ]]
  builtins.c    the shell's own commands
  jobs.c        background jobs
  trap.c        signals
  regex.c       the regex engine shared by grep, sed, awk and [[ =~ ]]
  line.c        the line editor
  history.c complete.c prompt.c config.c
  update.c      crish update
  gnu/
    gnu.c       the table of built-in utilities
    grep.c sed.c awk.c
    filters.c   sort uniq cut tr shuf xargs expr
    coreutils.c cat head tail wc rev tac nl paste tee seq yes basename
                dirname env sleep truncate
    datetime.c  date timeout
    hash.c      sha*sum md5sum base64
    fileinfo.c  stat readlink realpath mktemp
```

About 25,000 lines of C11.  No generated code, no dependencies.

## Adding a built-in utility

1. Write `int gnu_yourtool(int argc, char **argv)` in the right `src/gnu/` file.
2. Declare it in `src/gnu/gnu.h`.
3. Add a row to the `tools[]` table in `src/gnu/gnu.c` — **keep it sorted**,
   the lookup is a binary search.
4. Add checks to `tests/cases/07-gnu-tools.crsh` that pass under real GNU
   coreutils too.
5. Add a section to [docs/gnu.md](gnu.md) with **Works** and
   **Not implemented**.

Step 5 is not optional. The value of this project is that the compatibility
page is true.
