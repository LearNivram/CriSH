#!/bin/sh
# bench.sh - the numbers in the README and in docs/benchmarks.md.
#
# Every figure is the mean of N runs of the same work, measured from this
# script so that the shell under test does the measuring as little as possible.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu

cd "$(dirname "$0")/.."
CRISH=${CRISH:-./build/crish}
N=${N:-200}
[ -x "$CRISH" ] || { echo "build it first: make" >&2; exit 1; }

now() { python3 -c 'import time; print(time.perf_counter())'; }

bench() {
	label=$1
	shift
	start=$(now)
	i=0
	while [ "$i" -lt "$N" ]; do
		"$@" >/dev/null 2>&1 || true
		i=$((i + 1))
	done
	end=$(now)
	python3 -c "print(f'  {'$label':<34} {($end - $start) * 1000 / $N:6.2f} ms')"
}

printf '\n  startup: run nothing and exit (%d runs each)\n\n' "$N"
bench "crish -c exit"        "$CRISH" -c exit
[ -x /bin/bash ] && bench "bash -c exit (macOS 3.2)" /bin/bash -c exit
[ -x /bin/zsh ]  && bench "zsh -c exit"             /bin/zsh -c exit
[ -x /bin/sh ]   && bench "sh -c exit"              /bin/sh -c exit
for extra in /opt/homebrew/bin/bash /usr/local/bin/bash; do
	[ -x "$extra" ] && bench "$(basename "$extra") -c exit (GNU 5.x)" "$extra" -c exit
done

PIPE='printf "%s\n" a b c d e f g h i j | grep -v z | sed "s/a/A/" | sort -r | head -3'
printf '\n  a four stage pipeline (%d runs each)\n\n' "$((N / 4))"
NSAVE=$N
N=$((N / 4))
bench "crish  (built-in utilities)"  "$CRISH" -c "$PIPE"
[ -x /bin/bash ] && bench "bash 3.2 (BSD utilities)" /bin/bash -c "$PIPE"
if [ -x /opt/homebrew/bin/bash ]; then
	bench "bash 5.x (whatever is on PATH)" /opt/homebrew/bin/bash -c "$PIPE"
elif [ -x /usr/local/bin/bash ]; then
	bench "bash 5.x (whatever is on PATH)" /usr/local/bin/bash -c "$PIPE"
fi
N=$NSAVE

printf '\n  binary\n\n'
printf '  %-34s %s\n' "size" "$(du -h "$CRISH" | cut -f1)"
printf '  %-34s %s\n' "architectures" "$(lipo -archs "$CRISH" 2>/dev/null || uname -m)"
printf '  %-34s %s\n' "dynamic libraries" \
	"$(otool -L "$CRISH" | tail -n +2 | wc -l | tr -d ' ')"
otool -L "$CRISH" | tail -n +2 | sed 's/^/    /'
printf '\n'
