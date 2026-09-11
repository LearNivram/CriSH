# Benchmarks

Every number in the README comes out of [`tools/bench.sh`](../tools/bench.sh).
Run it yourself:

```sh
make
./tools/bench.sh          # 200 runs per case
N=1000 ./tools/bench.sh   # more, if the machine is noisy
```

## Method

The loop runs the shell under test N times, measured from outside with
`time.perf_counter()`, and divides. That means the number includes `fork` and
`execve` of the shell itself, which is the honest thing to measure: what it
costs you to run a script.

It does not use `hyperfine` so that the script has no dependencies, and it does
not report a standard deviation because the distribution here is dominated by
the OS and is not usefully summarised by one. Run it twice if a number looks
odd.

## What was measured

macOS 26.6, Apple silicon, `-O2`, universal binary, warm page cache, nothing
else running.

### Startup

```
crish -c exit                        1.73 ms
bash -c exit (macOS 3.2)             1.48 ms
zsh -c exit                          2.18 ms
sh -c exit                           2.41 ms
bash -c exit (Homebrew 5.3)          5.75 ms
```

The comparison that matters is the last line. `/bin/bash` is fast because it is
bash 3.2 and cannot do what you need; the bash 5 you would install to get
associative arrays and `${v^^}` starts in 5.75 ms. CriSH gets you the same
language for 1.73 ms, and brings the GNU utilities with it.

### A four stage pipeline

`printf | grep -v | sed | sort -r | head -3`, 50 runs:

```
crish  (built-in utilities)          3.01 ms
bash 3.2 with BSD utilities          3.76 ms
bash 5.3 with GNU utilities          8.63 ms
```

A pipeline stage that is a built-in utility still forks — the stages have to
run concurrently — but it never `execve`s, and the code is already resident.
That is where the difference comes from. It grows with the number of stages.

### The binary

```
size                 720K
architectures        x86_64 arm64
dynamic libraries    1   (/usr/lib/libSystem.B.dylib)
```

## What is not optimised

- **Globbing** walks the directory tree without caching `stat` results.
  `**/*` over a huge tree is slower than bash's.
- **The regex engine** is a backtracking matcher with a step limit, not a DFA.
  It is fine for line-oriented work and can be made to backtrack badly by a
  pathological pattern; GNU grep's DFA will beat it on large inputs.
- **`sort`** reads everything into memory. There is no external merge, so a
  file larger than RAM will not sort.
- **Startup** has not been micro-optimised. There is no lazy symbol table and
  no attempt to avoid touching the environment. The 1.73 ms is mostly `execve`
  and dyld.

These are listed because a benchmark page that only shows what a project wins
is not worth reading.
