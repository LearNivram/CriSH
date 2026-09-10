# Colour

CriSH colours the things that carry information and leaves the rest alone.
The point is not decoration; it is that **you can see what exists before you
press Enter**.

```
$ grpe -n main src/*.c
  ^^^^ red: nothing by that name can be run

$ grep -n main src/*.c
  ^^^^ green: it resolves
```

## What gets a colour

### While you type

| | |
| --- | --- |
| a command name that resolves | green, and builtins, built-in GNU tools, functions and aliases each look slightly different |
| a command name that does not | **red** |
| `if while for case function` and friends | blue |
| `'…'` `"…"` `$'…'` | yellow, with escapes inside brighter |
| `$var` `${…}` `$(…)` `` `…` `` | cyan — and the command *inside* `$(…)` is checked like any other |
| `\| && \|\| ; &` | magenta |
| `< > >> 2>&1 <<<` | blue |
| `# a comment` | grey |
| `-f` `--flag` | dimmed |
| an argument that names a path that exists | **underlined** |
| `NAME=` at the head of a command | the name in cyan, the `=` in magenta |

The scanner is deliberately not the shell's parser: it copes with a half
written line, never reports an error and never runs anything. It also caches
what it learned about a name, because a failing `PATH` lookup walks every
directory on it and that would otherwise happen on every keystroke.

### Everywhere else

- **The prompt**: user, host, working directory and git branch each have a
  role, and the `$` turns red and grows the exit status when the last command
  failed.
- **The completion list**: directories, executables, symlinks, builtins,
  built-in tools, functions, aliases and variables are told apart at a glance.
- **Errors** in red, warnings in yellow, the file and line in grey.
- **`grep`** paints the match, the file name, the line number and the
  separators. See below.
- **`ls`** paints by file type, reading `LS_COLORS` when it is set.

## When colour is used

Checked in this order:

1. `--color=never`, `--no-color`, or **`NO_COLOR` set to anything at all**
   (including the empty string) → off. This is the
   [no-color.org](https://no-color.org) convention.
2. `--color=always` or `CLICOLOR_FORCE` → on, even into a pipe.
3. `TERM` unset or `dumb` → off.
4. The output is not a terminal → off.
5. Otherwise → on.

Rule 4 is why a script is never affected: `crish script.sh > out` writes exactly
the same bytes it always did, and it is also why the test suite never sees an
escape.

```sh
crish --color=never script.sh     # for one run
NO_COLOR=1 crish script.sh        # the same, by convention
shopt -u color                    # for the rest of this shell
shopt -u syntax_highlight         # keep colour, stop painting the line as you type
```

## Themes

```sh
theme                # list them, marking the current one
theme muted          # switch
theme --preview      # every role, in its colour
theme --roles        # the role names, for CRISH_COLORS
```

| | |
| --- | --- |
| `bold` | the default: the bright ANSI colours, legible on any terminal theme |
| `muted` | 256-colour, in the Solarized spirit; quieter, needs a 256-colour terminal |
| `mono` | no colour at all, only bold, dim and underline — existing commands bold, missing ones underlined |

Set one for good in `~/.crishrc`:

```sh
export CRISH_THEME=muted
```

## Changing individual colours

`CRISH_COLORS` overrides single roles on top of the current theme, in the shape
of `LS_COLORS`: `role=SGR` pairs separated by colons, where the SGR is what
goes between `ESC[` and `m`.

```sh
export CRISH_COLORS='missing=1;4;31:cmd=32:string=38;5;180:comment=2'
```

An empty value removes the colour for that role:

```sh
export CRISH_COLORS='flag='      # stop dimming flags
```

`theme --roles` prints every name. The useful ones:

```
cmd missing builtin gnu function alias keyword string escape var subst
operator redir comment flag path
dir exec link fifo sock block char file
error warn hint
match filename lineno sep
user host cwd git ok fail
```

## The prompt

The default `PS1` is built from roles, so a theme repaints it too:

```
\c{user}\u\c{}@\c{host}\h\c{} \c{cwd}\w\c{}\c{git}\G\c{} \P
```

Two escapes exist for this:

| | |
| --- | --- |
| `\c{role}` | start that role's colour; `\c{}` ends it |
| `\P` | the `$` (or `#` for root), green after a success, and red with the exit status in front after a failure |

A `PS1` you set yourself, or one inherited from the environment, is never
overwritten. Everything from bash works as before, including `\[` and `\]`.

## `grep`

```sh
grep --color=auto pattern file     # the default
grep --color=always pattern file   # even into a pipe
grep --color=never pattern file
```

`GREP_COLORS` is read, with the GNU names: `ms` (matching text), `mc`, `sl`
(selected line), `cx` (context line), `fn` (file name), `ln` (line number),
`se` (separator).

```sh
export GREP_COLORS='ms=01;36:fn=35:ln=32:se=36'
```

**One deliberate difference from GNU:** CriSH's `grep` defaults to
`--color=auto`, GNU's defaults to `never`. Almost every Linux setup aliases
`grep` to `--color=auto`, so this matches what people actually see, and because
`auto` means "only when the output is a terminal", no script's output changes.

## `ls`

`LS_COLORS` is read when set — the type keys (`di ln ex fi pi so bd cd or`) and
`*.suffix` rules. Without it, CriSH's own theme applies. Ordinary files get no
escape at all, as with GNU.

```sh
ls --color=auto            # the default
ls --color=always | cat    # colour into a pipe
eval "$(dircolors -b)"     # if you have a dircolors from somewhere
```

## Turning all of it off

```sh
export NO_COLOR=1
```

That one line is enough: it turns off the editor, the prompt, the diagnostics,
`grep` and `ls` together.
