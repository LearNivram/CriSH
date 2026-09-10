#!/bin/sh
# install.sh - install CriSH on macOS.
#
#   sh -c "$(curl -fsSL https://raw.githubusercontent.com/LearNivram/CriSH/master/install.sh)"
#
# Downloads the newest release, checks its SHA-256 against the checksums file
# published with it, and installs the binary. Nothing else on the system is
# touched: no PATH surgery, no shell configuration, no /etc/shells entry unless
# you ask for one with --register.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu

REPO=${CRISH_REPO:-LearNivram/CriSH}
ASSET=crish-macos-universal
PREFIX=${PREFIX:-}
VERSION=${CRISH_VERSION:-}
REGISTER=0

say()  { printf '\033[1mcrish\033[0m: %s\n' "$1"; }
warn() { printf '\033[33mcrish\033[0m: %s\n' "$1" >&2; }
die()  { printf '\033[31mcrish\033[0m: %s\n' "$1" >&2; exit 1; }

usage() {
	cat <<'USAGE'
usage: install.sh [options]

  --prefix DIR    install into DIR/bin (default: /usr/local, or ~/.local
                  when /usr/local is not writable)
  --version TAG   install a specific release instead of the newest
  --register      add the installed shell to /etc/shells (needs sudo)
  --uninstall     remove an installed crish
  --help          show this message
USAGE
}

uninstall() {
	found=0
	for dir in /usr/local/bin "$HOME/.local/bin" /opt/homebrew/bin; do
		if [ -x "$dir/crish" ]; then
			say "removing $dir/crish"
			rm -f "$dir/crish" 2>/dev/null || sudo rm -f "$dir/crish"
			found=1
		fi
	done
	[ "$found" -eq 1 ] || warn "no installed crish found"
	exit 0
}

while [ $# -gt 0 ]; do
	case $1 in
	--prefix)    PREFIX=$2; shift 2 ;;
	--prefix=*)  PREFIX=${1#*=}; shift ;;
	--version)   VERSION=$2; shift 2 ;;
	--version=*) VERSION=${1#*=}; shift ;;
	--register)  REGISTER=1; shift ;;
	--uninstall) uninstall ;;
	--help|-h)   usage; exit 0 ;;
	*)           die "unknown option $1" ;;
	esac
done

[ "$(uname -s)" = Darwin ] || die "CriSH is for macOS; on Linux you already have GNU tools"
command -v curl >/dev/null || die "curl is required"

if [ -z "$PREFIX" ]; then
	if [ -w /usr/local/bin ] || [ -w /usr/local ]; then
		PREFIX=/usr/local
	else
		PREFIX=$HOME/.local
	fi
fi
BINDIR=$PREFIX/bin

if [ -z "$VERSION" ]; then
	say "looking up the newest release"
	VERSION=$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" |
		sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' |
		head -1)
	[ -n "$VERSION" ] || die "cannot reach the GitHub API; pass --version TAG"
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/crish-install-XXXXXX")
trap 'rm -rf "$TMP"' EXIT INT TERM

BASE="https://github.com/$REPO/releases/download/$VERSION"
say "downloading $VERSION"
curl -fsSL -o "$TMP/$ASSET" "$BASE/$ASSET" ||
	die "no $ASSET in release $VERSION"

if curl -fsSL -o "$TMP/checksums.txt" "$BASE/checksums.txt" 2>/dev/null; then
	want=$(grep "$ASSET" "$TMP/checksums.txt" | awk '{print $1}' | head -1)
	have=$(shasum -a 256 "$TMP/$ASSET" | awk '{print $1}')
	if [ -n "$want" ] && [ "$want" != "$have" ]; then
		die "checksum mismatch
  expected $want
  got      $have"
	fi
	say "checksum verified"
else
	warn "no checksums.txt in this release; skipping verification"
fi

chmod 0755 "$TMP/$ASSET"
mkdir -p "$BINDIR" 2>/dev/null || sudo mkdir -p "$BINDIR"
if [ -w "$BINDIR" ]; then
	mv "$TMP/$ASSET" "$BINDIR/crish"
else
	say "$BINDIR needs elevated rights"
	sudo mv "$TMP/$ASSET" "$BINDIR/crish"
fi

if [ "$REGISTER" -eq 1 ]; then
	if ! grep -qx "$BINDIR/crish" /etc/shells 2>/dev/null; then
		say "adding $BINDIR/crish to /etc/shells"
		printf '%s\n' "$BINDIR/crish" | sudo tee -a /etc/shells >/dev/null
	fi
	say "run 'chsh -s $BINDIR/crish' to make it your login shell"
fi

say "installed $("$BINDIR/crish" --version) to $BINDIR/crish"
case ":$PATH:" in
*":$BINDIR:"*) ;;
*) warn "$BINDIR is not on your PATH; add it to use \`crish\` by name" ;;
esac
say "try: crish -c 'declare -A m; m[k]=v; echo \${m[k]}'"
