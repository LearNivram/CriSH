#!/bin/sh
# build.sh - build a universal (arm64 + x86_64) CriSH binary.
#
# clang on macOS emits both slices from one invocation, so there is no lipo
# step and no second object tree.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu

CC=${CC:-cc}
CFLAGS=${CFLAGS:--O2}
OUT=${OUT:-build/crish}
ARCHS=${ARCHS:-"arm64 x86_64"}

cd "$(dirname "$0")"
mkdir -p "$(dirname "$OUT")"

arch_flags=""
for a in $ARCHS; do
	arch_flags="$arch_flags -arch $a"
done

# shellcheck disable=SC2086
set -- $arch_flags

printf 'building %s for%s\n' "$OUT" "$(printf ' %s' $ARCHS)"
$CC $CFLAGS -std=c11 -D_DARWIN_C_SOURCE \
	-Wall -Wextra -Wno-unused-parameter \
	"$@" \
	-o "$OUT" src/*.c src/gnu/*.c

file "$OUT"
printf 'size: %s\n' "$(du -h "$OUT" | cut -f1)"
