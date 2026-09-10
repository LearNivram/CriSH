# GNU compatibility

This is the page that matters. It says, per command, what CriSH's built-in
version does, where it deliberately differs from the BSD version macOS ships,
and what it does **not** implement.

Everything under **Works** has a check in [`tests/cases/`](../tests/cases), and
those same checks run against real GNU coreutils on Linux in CI. If a line here
is wrong, the build goes red.

## How the built-ins are chosen

When you type `sed`, CriSH resolves the name in this order:

1. a shell function
2. a shell builtin (`echo`, `printf`, `cd`, `test`, …)
3. **a built-in GNU-compatible utility** (this page)
4. the `PATH`

So a built-in shadows `/usr/bin/sed`, and nothing else on the system changes.
Two escape hatches:

```sh
command sed ...              # ignore shell functions; the built-in still wins
command -p sed ...           # ignore the built-in too and run /usr/bin/sed
shopt -u gnu_builtins        # turn every built-in utility off for this shell
crish --posix script.sh      # start a shell with them all off
```

With `gnu_builtins` off, CriSH is a bash-compatible shell using the BSD tools
in `/usr/bin`, which is occasionally what you want when a script deliberately
targets BSD behaviour.

Anything not on this page — `ls`, `cp`, `mv`, `rm`, `mkdir`, `ln`, `touch`,
`chmod`, `find`, `git`, everything else — is run from the `PATH` unchanged.
The BSD versions of those are close enough that shadowing them would add risk
without adding value.

---

## The commands that do not exist on macOS at all

### `timeout`

macOS has no `timeout`. This one does.

**Works:** `timeout DURATION COMMAND [ARG]...`, `-s`/`--signal`,
`-k`/`--kill-after`, `--preserve-status`, `--foreground` (accepted; CriSH
already runs the child in the foreground), suffixes `s` `m` `h` `d`, fractional
durations, exit status `124` on timeout and `125` on a usage error.

**Not implemented:** killing a whole process group when the child spawns its
own; `--signal` names outside the standard set.

### `sha256sum`, `sha1sum`, `sha512sum`, `md5sum`

macOS has `shasum -a 256` with different output and no `md5sum` at all.

**Works:** the GNU output format (`HASH  FILE`, two spaces), `-`/stdin,
`-c`/`--check` including `FAILED` lines and the warning summary, `-b`, `-q`,
`--status`, `--tag` (BSD-style output).

**Not implemented:** `--ignore-missing`, `--strict`, `-z`.

The digests come from CommonCrypto, which is part of `libSystem`, so no
cryptography is reimplemented here.

---

## The commands where GNU and BSD differ enough to break scripts

### `sed`

The one that silently destroys files on a Mac.

```sh
sed -i 's/a/b/' file       # GNU: edit in place.  BSD: "-i" takes 's/a/b/'
                           #      as the backup suffix and then fails.
```

**Works:** `-i[SUFFIX]` with the suffix **attached**, never taken from the next
argument · `-n` · `-e` · `-f` · `-E`/`-r` · `-s` · `-z` · `--posix`,
`--unbuffered`, `--follow-symlinks`, `--debug`, `--sandbox` accepted and ignored.

Addresses: `N`, `$`, `/re/`, `\cREc`, `first~step`, `addr1,addr2`, `addr,+N`,
`addr,~N`, `0,/re/`, and `!` negation. Address regex flags `I` and `M`.

Commands: `s` `y` `p` `P` `d` `D` `n` `N` `h` `H` `g` `G` `x` `z` `=` `l` `q`
`Q` `a` `i` `c` `r` `w` `W` `b` `t` `T` `:` and `{ }` blocks.

`s///` flags: `g`, `p`, `i`/`I`, `m`/`M`, a repetition number, `w FILE`.
Replacement text understands `&`, `\1`–`\9`, `\n` `\t` `\r`, and the GNU case
operators `\U \L \u \l \E`.

**Not implemented:** `e` (execute the pattern space), `F` prints the file name
but not for stdin, `--line-length`/`l N` wrapping, `R`.

### `date`

BSD `date` has no `-d`, no `--iso-8601`, and no `%N`.

**Works:** `-d`/`--date` with `@EPOCH`, `now`, `today`, `tomorrow`,
`yesterday`, `midnight`, `noon`, `YYYY-MM-DD[ HH:MM[:SS]]`, `MM/DD/YYYY`,
`HH:MM[:SS]`, `N unit`, `+N unit`, `-N unit`, `N unit ago`, `next unit`,
`last unit`, and combinations of an absolute date with an offset ·
`-u`/`--utc` · `-r`/`--reference FILE` · `-R`/`--rfc-email` ·
`--iso-8601[=date|hours|minutes|seconds|ns]` and `-I[...]` ·
`--rfc-3339=...` · `+FORMAT`.

Format extensions beyond `strftime`: `%s`, `%N`, `%:z`, `%::z`, and the GNU
padding flags `%-`, `%_`, `%^`.

**Not implemented:** setting the clock (`-s`), day-of-week phrases
(`next monday`), `TZ=` inside the date string, `--debug`.

### `stat`

BSD `stat` uses `-f` with a different format language and has no `-c`.

**Works:** `-c`/`--format`/`--printf` with `%n %N %s %b %B %f %a %A %u %U %g %G
%i %h %d %D %F %X %Y %Z %W %x %y %z %w %%`, `-L`/`--dereference`, `-t`/`--terse`,
and a default long form close to GNU's.

**Not implemented:** `-f` file-system status, `%m` mount point, `%C` SELinux.

### `readlink` and `realpath`

**Works:** `readlink -f`, `-e`, `-m`, `-n`, `-z`, `-q`/`-s`, and the
`--canonicalize*` long forms · `realpath` with `--relative-to`, `-m`, `-q`,
`-z`, `-s` (accepted).

**Not implemented:** `realpath --relative-base`, `--logical`.

### `grep`

**Works:** `-E` `-F` `-G` `-P` · `-i` `-v` `-w` `-x` · `-c` `-l` `-L` `-q`
`-n` `-h` `-H` `-o` `-b` `-Z` · `-r`/`-R` · `-s` · `-a` · `-m N` ·
`-A N` `-B N` `-C N` and the bare `-N` form · `-e PATTERN` (repeatable) ·
`-f FILE` · `--include` `--exclude` `--exclude-dir` · `--label` ·
`--color[=auto|always|never]` with `GREP_COLORS` (`ms mc sl cx fn ln se`) ·
exit status 0/1/2 · `egrep` and `fgrep` when invoked under those names.

`-P` is a Perl **subset**: `\d \D \w \W \s \S \b \B`, non-greedy `*? +? ?? {}?`,
non-capturing `(?:...)`. Look-around, named groups, `\K`, possessive quantifiers
and Unicode properties are **not implemented** and produce a pattern error
rather than a wrong answer.

**Not implemented:** `--binary-files` beyond being accepted, `-z`,
`--devices`, `--group-separator`.

**One deliberate difference:** the default is `--color=auto`, where GNU's is
`never`. Nearly every Linux setup aliases `grep` to `--color=auto`, and `auto`
paints only when the output is a terminal, so no script's output changes. See
[colours](colors.md#grep).

### `ls`

BSD `ls` has no `--color`, no `--group-directories-first` and no
`--time-style` — exactly the flags a Linux dotfile reaches for.

**Works:** `-l -a -A -1 -C -d -f -F -g -G -h -i -k -n -o -p -Q -r -R -S -t -u
-c -U -v -X` · `--color[=WHEN]` · `--group-directories-first` ·
`--time-style=full-iso|long-iso|iso|+FORMAT` ·
`--sort=none|size|time|extension|version` · the long forms `--all`,
`--almost-all`, `--reverse`, `--recursive`, `--human-readable`, `--classify`,
`--inode`, `--numeric-uid-gid`, `--directory`, `--quote-name`.

`LS_COLORS` is read when set (the `di ln ex fi pi so bd cd or` keys and
`*.suffix` rules); otherwise CriSH's own theme applies. One entry per line when
the output is not a terminal, columns when it is, as GNU does. Ordinary files
get no escape at all.

**Not implemented:** `-Z` SELinux contexts, `--dired`, ACL and extended
attribute columns (no `+` or `@` marker after the mode), `--hyperlink`,
`--block-size`, `-s`, `--author`, and locale-aware collation — sorting is
byte-wise, as everywhere in CriSH. `-R` walks breadth first, so the directory
headers come in a slightly different order from GNU's depth-first walk.

### `awk`

A POSIX awk. See [awk](awk.md) for the language details.

**Works:** `BEGIN`/`END`, bare and range patterns, `/re/` patterns, user
functions, arrays (including `(i, j) in a` and `delete`), `getline` in its
plain, `var`, `< file` and `cmd |` forms, `print` and `printf` with `>`, `>>`
and `| cmd` redirection, `-F`, `-v`, `-f`, `--`, `var=value` operands between
files, and `NR NF FNR FS OFS ORS RS SUBSEP FILENAME RSTART RLENGTH CONVFMT OFMT
ENVIRON ARGC ARGV`. `RS` as a single character, empty (paragraph mode) or a
regular expression.

Built-ins: `length substr index split sub gsub match sprintf sin cos atan2 exp
log sqrt int rand srand tolower toupper system close fflush`.

**Not implemented:** gawk extensions — `asort`, `asorti`, `gensub`,
`strftime`, `systime`, `mktime`, `patsplit`, `BEGINFILE`/`ENDFILE`,
`nextfile` inside a function, `--profile`, `-M` arbitrary precision.

### `sort`

**Works:** `-n` `-g` `-h` `-V` `-M` (accepted) · `-r` `-f` `-b` `-u` `-s`
`-c` `-z` `-R` · `-k F[.C][opts][,F[.C][opts]]` with per-key modifiers ·
`-t SEP` · `-o FILE` · the long forms of all of these ·
`--parallel`, `--buffer-size`, `--temporary-directory` accepted and ignored.

Sorting is stable (mergesort), which is what `-s` promises and what GNU does
when `-s` is given.

**Not implemented:** `--files0-from`, `--compress-program`, `--debug`, and
locale-aware collation — comparison is byte-wise, which is `LC_ALL=C` order.
Set `LC_ALL=C` in scripts you want to behave identically everywhere; GNU sort
in a UTF-8 locale will differ.

### `head` and `tail`

**Works:** `head -n N`, `head -n -N` (all but the last N), `head -c N`,
`head -c -N`, the historical `head -5` · `tail -n N`, `tail -n +N`,
`tail -c N`, `tail -c +N`, `tail -f` · `-q` `-v` and the long forms ·
size suffixes `b k K m M g G`.

**Not implemented:** `tail --pid`, `tail -F` differs from `-f` only in name,
`head -z`.

### `cut`, `tr`, `uniq`, `paste`, `nl`, `wc`, `tac`, `rev`, `shuf`, `seq`

**Works:**

- `cut`: `-d` `-f` `-c` `-b` `-s` `-z` `--complement` `--output-delimiter`
- `tr`: `-d` `-s` `-c`/`-C` `-t`, ranges, `[:classes:]`, `\n \t \\ \NNN` escapes
- `uniq`: `-c` `-d` `-D` `-u` `-i` `-f N` `-s N` `-w N` `-z`
- `paste`: `-d` (with escape sequences), `-s`, combined short options
- `nl`: `-b a|t|n` `-w` `-s` `-v` `-i`
- `wc`: `-l` `-w` `-c` `-m` `-L`, a `total` line for several files
- `tac`: `-s`
- `shuf`: `-n` `-e` `-i LO-HI` `-r` `-z`
- `seq`: `-w` `-s` `-f`, one, two or three operands, negative steps

**Not implemented:** `cut --output-delimiter` for `-c`/`-b` beyond a single
character, `tr` equivalence classes `[=c=]`, `nl -h`/`-f` section logic,
`shuf --random-source`.

### `xargs`

**Works:** `-0`/`--null` · `-n N` · `-I REPLACE` · `-r`/`--no-run-if-empty` ·
`-t`/`--verbose` · `-d DELIM` · quote and backslash handling in the default
word splitting · `-P N` accepted (commands run serially).

**Not implemented:** real parallelism for `-P`, `-s` size limits, `-a`,
`--process-slot-var`.

### `base64`

**Works:** encode and decode, `-d`/`--decode`, `-w N`/`--wrap`,
`-i`/`--ignore-garbage`, reading from a file or stdin. `-w0` produces one
unwrapped line with **no** trailing newline, matching GNU.

**Not implemented:** `base32`, `basenc`.

### `mktemp`

**Works:** `-d` `-u` `-q` `-t` `-p DIR` `--suffix=S`, templates with `XXX`.

**Not implemented:** GNU's exact template validation messages.

### `env`, `sleep`, `basename`, `dirname`, `expr`, `truncate`, `yes`, `cat`, `tee`

**Works:**

- `env`: `-i` `-u NAME` `--`, `VAR=value` prefixes, printing the environment
- `sleep`: fractional seconds and `s m h d` suffixes, several operands summed
- `basename`: `-a` `-s SUFFIX` `-z`, and the two-operand form
- `dirname`: `-z`, several operands
- `expr`: arithmetic, comparisons, `&` `|`, `length` `substr` `index`, `:` match
- `truncate`: `-s N`, `-s +N`, `-s -N`, size suffixes
- `yes`, `cat -n -b -s -A -E -T -v -e -t`, `tee -a -i`

**Not implemented:** `env -S`, `env --chdir`, `expr` on very large integers
(values are `long`), `truncate -r`/`-o`.

---

## Things that are deliberately not GNU

- **Locale.** CriSH does not call `setlocale`. Comparison, case conversion and
  number formatting behave as `LC_ALL=C`. This makes scripts reproducible
  across machines; it also means `sort` will not match GNU sort running under a
  UTF-8 locale. Set `LC_ALL=C` on both sides when that matters.
- **Regex matching is leftmost-longest** (POSIX) for BRE and ERE, and
  leftmost-first (Perl) for `-P`, which is what GNU does.
- **`--help` output** is short and lists what CriSH supports rather than
  reproducing GNU's manual.
- **`--version`** is not accepted by most of the built-ins; `crish --version`
  reports the one version that matters.

## Turning it off

```sh
shopt -u gnu_builtins                 # for the rest of this shell
crish --posix script.sh               # for one script
command -p sed -i '' file             # for one command
```

`shopt gnu_builtins` is on by default. `type sed` tells you which one you are
about to get.
