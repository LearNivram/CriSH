# bash compatibility

What "runs Linux shell scripts unchanged" means, precisely. Everything under
**Works** has a check in [`tests/cases/`](../tests/cases) that also runs against
real bash 5 in CI.

CriSH targets the **bash 5 language**, not POSIX sh and not the bash 3.2 that
macOS ships. A `#!/bin/bash` script that runs on Ubuntu should run here.

## Works

### Expansion

| | |
| --- | --- |
| Parameters | `$v` `${v}` `${v:-w}` `${v:=w}` `${v:?w}` `${v:+w}` and the colon-less forms |
| Length | `${#v}` `${#a[@]}` `${#@}` |
| Trimming | `${v#pat}` `${v##pat}` `${v%pat}` `${v%%pat}` |
| Replacement | `${v/pat/rep}` `${v//pat/rep}` `${v/#pat/rep}` `${v/%pat/rep}` |
| Slicing | `${v:off}` `${v:off:len}`, negative offsets and lengths |
| Case | `${v^}` `${v^^}` `${v,}` `${v,,}` with an optional pattern |
| Transform | `${v@Q}` `${v@U}` `${v@L}` `${v@E}` `${v@A}` `${v@a}` `${v@P}` |
| Indirection | `${!ref}`, `${!prefix*}`, `${!prefix@}` |
| Arrays | `${a[i]}` `${a[@]}` `${a[*]}` `${!a[@]}` `${a[-1]}` |
| Arithmetic | `$(( ))` with the whole C operator set, `**`, `16#ff`, `0x1f`, `0b1010` |
| Command | `$( )` and `` ` ` ``, nested |
| Process | `<( )` and `>( )` |
| Brace | `{a,b}` `{1..10}` `{1..10..2}` `{a..z}` `{01..12}` |
| Tilde | `~` `~user` `~+` `~-` |
| Globs | `*` `?` `[...]` `[[:class:]]`, `**` with `globstar`, `?()` `*()` `+()` `@()` `!()` with `extglob` |

### Arrays

Indexed and **associative**, which `/bin/bash` on macOS cannot do at all:

```sh
declare -A count
for w in "$@"; do count[$w]=$(( ${count[$w]:-0} + 1 )); done
for k in "${!count[@]}"; do printf '%s\t%d\n' "$k" "${count[$k]}"; done
```

`a=(x y z)`, `a+=(w)`, `a[5]=sparse`, `unset 'a[1]'`, `${a[@]:1:2}`,
`declare -a`, `declare -A`, `readarray`/`mapfile`.

### Control flow

`if`/`elif`/`else`, `while`, `until`, `for x in`, `for ((;;))`, `case` with
`;;` `;&` `;;&`, `select`, `{ }`, `( )`, functions in both `name()` and
`function name` forms, `break N`, `continue N`, `return`, recursion.

### Conditionals

`test`, `[ ]` and `[[ ]]` including `=~` with `BASH_REMATCH`, glob `==`/`!=`,
`<`/`>` string comparison, `-v`, `-o`, `&&`/`||`/`!`/parentheses, and all the
usual file predicates.

### Redirection

`<` `>` `>>` `>|` `<>` `2>` `&>` `&>>` `>&n` `<&n` `<&-` `<<` `<<-` `<<<`
`|` `|&` `{fd}>`, `exec 3>file`, process substitution.

### Options

`set -e -u -x -v -f -n -m -C -a -h -o pipefail`, and `shopt` for
`globstar nullglob failglob dotglob extglob nocasematch nocaseglob
inherit_errexit lastpipe expand_aliases checkwinsize cmdhist histappend autocd
cdspell xpg_echo huponexit progcomp sourcepath interactive_comments`, plus two
of CriSH's own: `gnu_builtins` and `gnu_warn`.

### Builtins

`: . source alias unalias bg break builtin cd command continue declare typeset
dirs echo eval exec exit export false fg getopts hash help history jobs kill
let local mapfile popd printf pushd pwd read readarray readonly return set
shift shopt test [ times trap true type ulimit umask unalias unset update wait`

### Variables set for you

`PWD OLDPWD IFS PS1 PS2 PS4 PATH HOME SHLVL UID EUID PPID HOSTNAME RANDOM
SRANDOM SECONDS EPOCHSECONDS EPOCHREALTIME LINENO BASHPID BASH_REMATCH
OPTIND OPTARG REPLY CRISH_VERSION`

`EPOCHSECONDS`, `EPOCHREALTIME` and `SRANDOM` are bash 5 additions that macOS's
bash does not have.

## Not implemented

Listed here rather than left for you to find:

- **Coprocesses** (`coproc`). No plans; they are rare in scripts.
- **`bind`** and readline keymap programming. The line editor has fixed emacs
  bindings; see [shortcuts](shortcuts.md).
- **Restricted shell** (`crish -r`, `set -r`).
- **`declare -n` nameref** is parsed and stored but only resolves one level.
- **History expansion** (`!!`, `!$`, `^a^b`). `set -H` is accepted and ignored.
- **`trap` on `RETURN` and `DEBUG`** are accepted but never fire; `EXIT`,
  `ERR` and the real signals do.
- **Programmable completion** (`complete`, `compgen`). Completion works, but it
  is built in rather than scriptable.
- **`ulimit`** covers `-a -c -f -n -s -u -v -H -S` and nothing else.
- **`shopt -s lastpipe`** is accepted; the last stage of a pipeline still runs
  in a subshell.
- **Locale-aware collation and case conversion.** CriSH behaves as `LC_ALL=C`;
  see the note in [GNU compatibility](gnu.md#things-that-are-deliberately-not-gnu).
- **`$'...'`** supports `\u`/`\U` up to four and eight hex digits and encodes
  UTF-8, but no other multibyte handling is locale-aware.

## Differences you might notice

- `echo` follows bash: `-n`, `-e`, `-E`, and no escape processing by default.
  `shopt -s xpg_echo` switches to the other behaviour.
- `$0` inside `crish -c 'cmd' name args` is `name`, as in bash.
- Job control is present (`&`, `jobs`, `fg`, `bg`, `wait`, Ctrl-Z), but the
  terminal-handoff is simpler than bash's; long-running interactive job
  juggling is the least exercised part of CriSH.
- A syntax error names the file and line and stops the script, the same way
  bash does. See [how it fails](errors.md).

## Running CriSH as `sh`

CriSH does not pretend to be `/bin/sh`. If a script needs strict POSIX
behaviour, run it with `/bin/sh`. `crish --posix` turns off the GNU built-ins
and sets the `posix` option, but the language stays bash-flavoured.
