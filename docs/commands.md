# Command reference

Everything CriSH answers to, in one place. `help` prints the same list;
`help NAME` prints one entry.

## Shell builtins

| | |
| --- | --- |
| `:` | do nothing, successfully |
| `.` / `source` | read and run a file in this shell |
| `alias` / `unalias` | define, list or remove aliases |
| `bg` / `fg` / `jobs` / `wait` / `kill` | job control |
| `break` / `continue` | leave or restart a loop, with an optional level |
| `builtin` | run a builtin, ignoring functions |
| `cd` | change directory; `-L` `-P`, `cd -`, `CDPATH`, `shopt cdspell` |
| `command` | run a command ignoring functions; `-v` `-V`; `-p` also ignores the built-in utilities |
| `declare` / `typeset` | declare variables; `-a -A -i -l -u -r -x -t -n -g -p -f` |
| `dirs` / `pushd` / `popd` | the directory stack |
| `echo` | `-n` `-e` `-E` |
| `eval` | run arguments as a command |
| `exec` | replace the shell, or apply redirections permanently |
| `exit` / `return` | leave the shell, or a function |
| `export` / `readonly` | attributes on variables |
| `getopts` | parse option arguments |
| `hash` | remember or forget command locations; `-r` |
| `help` | describe builtins |
| `history` | show or clear the history |
| `let` | evaluate arithmetic |
| `local` | function-local variables |
| `mapfile` / `readarray` | read lines into an array; `-t -n -s -O -d -u` |
| `printf` | format and print; `-v var`, `%b`, `%q`, format recycling |
| `pwd` | `-L` `-P` |
| `read` | one line; `-r -s -p -d -n -N -a -t` and combined forms |
| `set` | options and positional parameters |
| `shift` | drop leading positional parameters |
| `shopt` | shell behaviour options |
| `test` / `[` | evaluate a conditional expression |
| `theme` | choose the colour scheme; `--preview`, `--roles` |
| `times` | accumulated process times |
| `trap` | run a command on a signal; `-p` `-l` |
| `true` / `false` | succeed, fail |
| `type` | how a name would be interpreted; `-t -p -P -a` |
| `ulimit` | resource limits; `-H -S -a -c -f -n -s -u -v` |
| `umask` | the file creation mask; `-S` |
| `unset` | remove variables or functions; `-v` `-f` |
| `update` | update CriSH itself |

## Built-in GNU-compatible utilities

Each of these shadows the one in `/usr/bin`. What is and is not implemented is
per command in [GNU compatibility](gnu.md).

```
awk       base64    basename  cat       cut       date      dirname
env       expr      grep      head      ls        md5sum    mktemp
nl        paste     readlink  realpath  rev       sed       seq
sha1sum   sha256sum sha512sum shuf      sleep     sort      stat
tac       tail      tee       timeout   tr        truncate  uniq
wc        xargs     yes
```

`type NAME` says which one you will get:

```
$ type sed
sed is a shell builtin (CriSH GNU-compatible)
$ type ls
ls is /bin/ls
```

## Command line

```
crish [options] [script [args]]
crish -c 'command' [name [args]]
```

| | |
| --- | --- |
| `-c CMD` | run CMD and exit |
| `-s` | read commands from standard input |
| `-i` | force interactive behaviour |
| `-l`, `--login` | act as a login shell |
| `-e -u -x -v -f -n -m -C -a -h` | the usual `set` options |
| `-o NAME` | a long option, such as `pipefail` |
| `--norc` | do not read `~/.crishrc` |
| `--color=WHEN` | colour output: `auto` (default), `always`, `never` |
| `--no-color` | the same as `--color=never` |
| `--highlight LINE` | print one line the way the editor would colour it |
| `--posix` | set the `posix` option and turn the GNU built-ins off |
| `--version` | print the version |
| `--help` | print the usage |

## `crish update`

| | |
| --- | --- |
| `crish update` | install the newest release |
| `crish update --check` | report whether one exists, install nothing |
| `crish update --pre` | consider prereleases |
| `crish update --selector` | list every release and pick one |

The download is checked against the release's `checksums.txt` and the binary is
replaced with `rename(2)`, so a failed update leaves the old one in place. If
CriSH lives somewhere you cannot write, the update says so instead of half
finishing.
