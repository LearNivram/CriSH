# Verifying a download

Every release ships a `checksums.txt` next to the binary. One command:

```sh
curl -fLO https://github.com/LearNivram/CriSH/releases/latest/download/crish-macos-universal
curl -fLO https://github.com/LearNivram/CriSH/releases/latest/download/checksums.txt
shasum -a 256 -c checksums.txt --ignore-missing
```

Expect `crish-macos-universal: OK`.

The installer does this for you and refuses to install on a mismatch. If the
release has no `checksums.txt`, the installer says so rather than pretending it
checked.

## What the checksum does and does not tell you

It tells you the bytes you have are the bytes GitHub served. It does not tell
you who built them. The binaries are built by the
[release workflow](../.github/workflows/release.yml) from a tagged commit, on
GitHub's own runners, and the workflow run is public — you can read the log
that produced any given release.

## Building it yourself instead

The strongest check is not to trust the download at all:

```sh
git clone https://github.com/LearNivram/CriSH
cd CriSH && git checkout v0.2.0
./build.sh
shasum -a 256 build/crish
```

The result will not be byte-identical to the released binary — clang embeds
paths and timestamps, and the runner's SDK differs from yours — so this is a
"build your own" story, not a reproducible-builds one. Making the build
reproducible is an open task.

## Code signing

Releases are currently **not** signed and **not** notarised, which is why a
browser download needs `xattr -d com.apple.quarantine`. This is stated here
rather than left to be discovered.
