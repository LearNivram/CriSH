# Configuration

CriSH reads one file, and only when it is interactive or a login shell:

```
~/.crishrc
```

Set `CRISHRC` to point somewhere else. `--norc` skips it. A login shell also
reads `/etc/crish/crishrc` and `~/.crish_profile` first.

There is no plugin system and no theme format. A `.crishrc` is a shell script.

## A reasonable starting point

```sh
# ~/.crishrc

# The prompt.  \g and \G are CriSH additions for the git branch.
PS1='\[\e[32m\]\u@\h\[\e[0m\] \[\e[34m\]\w\[\e[0m\]\[\e[33m\]\G\[\e[0m\] \$ '
PS2='> '

# History
HISTFILE=~/.crish_history
HISTSIZE=5000
HISTCONTROL=ignoredups:ignorespace
shopt -s histappend

# Behaviour
shopt -s globstar extglob nullglob
shopt -s autocd cdspell checkwinsize

alias ll='ls -lah'
alias gs='git status -sb'
```

## Prompt escapes

The bash set: `\u \h \H \w \W \$ \n \r \a \e \s \v \V \j \! \# \d \t \T \A \@
\D{fmt} \\ \nnn \[ \]`.

CriSH adds three:

| | |
| --- | --- |
| `\g` | the current git branch, or nothing |
| `\G` | ` (branch)`, or nothing |
| `\?` | the last exit status |

The branch is read straight out of `.git/HEAD`, walking up from the working
directory and following a worktree's `gitdir:` pointer. No `git` process is
started, which is why a git-aware prompt costs nothing measurable.

`$( )` and `$var` inside `PS1` are expanded every time the prompt is drawn, as
in bash.

## Variables CriSH reads

| | |
| --- | --- |
| `PS1` `PS2` `PS4` | prompts, and the `set -x` trace prefix |
| `PATH` `HOME` `IFS` | the usual |
| `HISTFILE` `HISTSIZE` `HISTCONTROL` | history; `HISTCONTROL` takes `ignoredups` and `ignorespace` |
| `CDPATH` | searched by `cd` |
| `PS3` | the `select` prompt |
| `TMPDIR` | where here-documents and `mktemp -t` go |
| `CRISHRC` | an alternative startup file |
| `LC_ALL` | not honoured; CriSH behaves as `LC_ALL=C` throughout |

## Two options of CriSH's own

```sh
shopt -u gnu_builtins   # stop shadowing sed, awk, grep, date and the rest
shopt -s gnu_warn       # print a note the first time a built-in shadows a PATH binary
```

Both default the other way: built-ins on, warnings off.
