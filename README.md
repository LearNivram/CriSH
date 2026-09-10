<p align="center">
  <img src="assets/logo.svg" width="112" alt="CriSH">
</p>

<h1 align="center">CriSH</h1>

<p align="center">
  <b>Runs Linux shell scripts on a Mac, unchanged.</b><br>
  No Homebrew, no <code>gnubin</code> on your <code>PATH</code>, no <code>brew install bash coreutils gnu-sed gawk</code>.<br>
  One universal binary, written in C, with the GNU behaviour built in.
</p>

```sh
sh -c "$(curl -fsSL https://raw.githubusercontent.com/LearNivram/CriSH/master/install.sh)"
```

Or grab `crish-macos-universal` from the [releases](https://github.com/LearNivram/CriSH/releases)
and put it on your `PATH`. Nothing else is needed. Every way to install it is in
[installing](docs/installing.md), and [verifying the download](docs/verifying.md)
takes one command.

## The problem

macOS can run shell scripts. It just cannot run *your* shell scripts. Here is a
stock macOS 26, with nothing installed:

| What a Linux script does | What a stock Mac says |
| --- | --- |
| `/bin/bash --version` | **3.2.57, from 2007** — Apple froze it at the last GPLv2 release |
| `declare -A m` | `declare: -A: invalid option` |
| `mapfile -t a < f` | `mapfile: command not found` |
| `echo "${v^^}"` | `bad substitution` |
| `shopt -s globstar` | `invalid shell option name` |
| `echo $EPOCHSECONDS` | *(empty)* |
| `sed -i 's/a/b/' f` | BSD `sed` eats the next argument as a backup suffix |
| `date -d '2 days ago'` | `illegal option -- d` |
| `timeout 5 cmd` | `timeout: command not found` |
| `stat -c %s f` | `illegal option -- c` |
| `sha256sum f` | `command not found` (it is `shasum -a 256`, with different output) |
| `readlink -f p` | works, but `-f` meant something else until recently |

The usual answer is `brew install bash coreutils gnu-sed gawk findutils` and then
prepending four `gnubin` directories to `PATH` in every shell you open. On your
own laptop, once. Not on a colleague's Mac, not in a CI runner, not on the box
you just ssh'd into.

## What CriSH is

One binary. `crish script.sh` runs the script with a bash-5 language and
GNU-behaving utilities, and nothing on the system changes.

```sh
$ /bin/bash -c 'declare -A m; m[k]=v; echo ${m[k]}'
/bin/bash: line 0: declare: -A: invalid option

$ crish -c 'declare -A m; m[k]=v; echo ${m[k]}'
v

$ echo hi > f; /bin/sed -i 's/hi/ho/' f
sed: 1: "f": invalid command code f

$ echo hi > f; crish -c "sed -i 's/hi/ho/' f"; cat f
ho
```

`awk`, `sed`, `grep`, `sort`, `date`, `timeout`, `sha256sum` and the rest are
**inside the binary** — the GNU versions' behaviour, not the BSD ones', and no
`execve` per pipeline stage. What is emulated and what is not is written down
per command in [the GNU compatibility page](docs/gnu.md), and every line under
**Works** there has a test.

## Honest about the tests

The suite in [`tests/`](tests/README.md) is **differential**. The same case files
run twice in CI:

- under `crish` on `macos-14` (arm64) and `macos-13` (x86_64),
- under **bash 5 with real GNU coreutils** on `ubuntu-latest`.

Any check that answers differently fails the build. That is the whole
compatibility claim, and it is mechanical rather than aspirational.

## Speed

Speed is not the point of CriSH, but it is worth knowing that the fix does not
cost you anything. The honest comparison is against **the bash 5 you would have
installed to get these features**:

| Shell | start, run nothing, exit |
| --- | --- |
| `bash -c exit` (`/bin/bash` 3.2, macOS) | 1.48 ms |
| **`crish -c exit`** | **1.73 ms** |
| `zsh -c exit` | 2.18 ms |
| `sh -c exit` | 2.41 ms |
| `bash -c exit` (Homebrew bash 5.3) | 5.75 ms |

A four stage pipeline, `printf | grep | sed | sort | head`:

| Shell | one run |
| --- | --- |
| **`crish`** (built-in utilities) | **3.01 ms** |
| `/bin/bash` 3.2 with BSD utilities | 3.76 ms |
| Homebrew bash 5.3 with GNU utilities | 8.63 ms |

A pipeline stage that is a built-in utility forks but never `execve`s, which is
where that difference comes from. The binary is 652 KB, universal (arm64 and
x86_64), and links nothing but `libSystem`. The method and the script behind
every number are in [benchmarks](docs/benchmarks.md).

## Documentation

**Using it:** [installing](docs/installing.md) ·
[configuration](docs/configuration.md) · [shortcuts](docs/shortcuts.md)

**Writing scripts:** [GNU compatibility](docs/gnu.md) ·
[bash compatibility](docs/bash.md) · [writing scripts](docs/scripting.md) ·
[command reference](docs/commands.md) · [awk](docs/awk.md) ·
[how it fails](docs/errors.md)

**Getting and trusting it:** [installing](docs/installing.md) ·
[verifying a download](docs/verifying.md)

**Working on it:** [building](docs/building.md) ·
[benchmarks](docs/benchmarks.md) · [tests](tests/README.md)

## Updating

CriSH updates itself:

```sh
crish update             # install the newest release
crish update --check     # only say whether there is one
crish update --pre       # take prereleases too
crish update --selector  # browse every release and pick one
```

## What it is not

CriSH is not a drop-in replacement for your interactive shell's plugin
ecosystem, and it is not trying to be `zsh`. It is a script runner with a
usable interactive mode. The things it does not do are listed plainly in
[bash compatibility](docs/bash.md) under **Not implemented**, rather than left
for you to discover.

## License

GNU General Public License v3.0, see [LICENSE](LICENSE).

## Contributing

Issues and pull requests welcome. [CONTRIBUTING.md](CONTRIBUTING.md) has the
branch naming, the commit style, what a change needs to bring with it, and how
releases are cut. The short version: branch as `feat/`, `fix/`, `perf/`,
`docs/`, `test/`, `ci/`, `refactor/` or `chore/` followed by kebab case, open a
pull request, keep CI green.
