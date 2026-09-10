# Interactive shortcuts

The line editor is built in: no readline, no terminfo, no configuration file.
The bindings are emacs-style and fixed.

## Moving

| | |
| --- | --- |
| `Ctrl-A` / `Home` | start of line |
| `Ctrl-E` / `End` | end of line |
| `Ctrl-B` / `←` | one character back |
| `Ctrl-F` / `→` | one character forward |
| `Alt-B` | one word back |
| `Alt-F` | one word forward |

## Editing

| | |
| --- | --- |
| `Backspace` | delete the character before the cursor |
| `Ctrl-D` | delete the character under the cursor, or exit on an empty line |
| `Ctrl-K` | delete to end of line |
| `Ctrl-U` | delete to start of line |
| `Ctrl-W` | delete the word before the cursor |
| `Alt-D` | delete the word after the cursor |
| `Ctrl-L` | clear the screen |

## History

| | |
| --- | --- |
| `↑` / `Ctrl-P` | previous command |
| `↓` / `Ctrl-N` | next command |
| `Ctrl-R` | reverse search; `Ctrl-R` again steps further back, `Enter` accepts, `Esc` cancels |

The line you were typing is kept while you browse history and comes back when
you walk past the newest entry.

## Completion

`Tab` completes. In command position it offers builtins, built-in utilities,
functions, aliases and everything executable on `PATH`; elsewhere it completes
paths, and a word starting with `$` completes variable names. One match is
inserted, several print in columns after inserting the longest common prefix.

## Signals

| | |
| --- | --- |
| `Ctrl-C` | abandon the current line, or interrupt the running command |
| `Ctrl-Z` | stop the running command; `fg` resumes it |
| `Ctrl-D` | exit, unless `set -o ignoreeof` |
