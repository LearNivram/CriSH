# Writing scripts for CriSH

The short version: write the script you would write for Linux.

```sh
#!/usr/bin/env crish
set -euo pipefail
```

That line is the whole story. Everything below is about the places where a
script written for macOS and a script written for Linux part company, and which
one CriSH follows.

## Portability, backwards

Most macOS portability advice is about making a Linux script survive BSD tools:

```sh
# the usual macOS dance
if sed --version >/dev/null 2>&1; then
  SED_INPLACE=(-i)
else
  SED_INPLACE=(-i '')
fi
sed "${SED_INPLACE[@]}" 's/a/b/' file
```

Under CriSH you delete that and write `sed -i 's/a/b/' file`.

The same goes for `date -d`, `stat -c`, `readlink -f`, `sha256sum`,
`timeout`, `head -n -1`, `sort -V`, `grep -P` and the rest of
[the list](gnu.md).

## Things worth doing anyway

**Set `LC_ALL=C` when the ordering matters.** CriSH behaves as `LC_ALL=C`
everywhere. GNU sort under a UTF-8 locale does not. Pinning the locale makes
both sides agree:

```sh
export LC_ALL=C
```

**Quote your expansions.** CriSH splits and globs exactly where bash does, which
means it also does not where bash does not:

```sh
for f in "$@"; do ...; done          # right
printf '%s\n' "${files[@]}"          # right
rm $file                             # wrong, here as everywhere
```

**Use `[[ ]]` for conditions and `[ ]` for portability.** Both work. `[[ ]]`
does not word-split its operands and has `=~`.

**Prefer `$( )` to backticks.** Both work; nesting only reads well one way.

## Making a script run everywhere

A script with `#!/usr/bin/env crish` needs CriSH installed. If you want one
script that runs on Linux and on a Mac with CriSH and on a Mac without it, the
honest shebang is still `#!/usr/bin/env bash`, and you invoke it as
`crish ./script.sh` when you want CriSH's behaviour:

```sh
crish ./deploy.sh --dry-run       # CriSH's tools
bash ./deploy.sh --dry-run        # whatever bash and PATH give you
```

Nothing in the script changes.

## Checking a script without running it

```sh
crish -n script.sh        # parse only; syntax errors, no side effects
crish -x script.sh        # trace every command after expansion
```

`crish -n` is worth putting in a pre-commit hook.

## Reading input safely

```sh
while IFS= read -r line; do
  printf '%s\n' "$line"
done < file
```

`IFS=` keeps leading and trailing whitespace, `-r` keeps backslashes. Both
behave as in bash.

For a whole file into an array, `mapfile` works here and does not exist on a
stock Mac:

```sh
mapfile -t lines < file
printf '%d lines\n' "${#lines[@]}"
```

## Temporary files

```sh
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
```

`trap ... EXIT` fires on a normal exit, on `exit N`, and on `INT`/`TERM` if you
trap those too.

## Pipelines and failure

```sh
set -o pipefail
if ! output=$(build 2>&1 | tee build.log); then
  printf 'build failed\n' >&2
  exit 1
fi
```

Without `pipefail`, the status is the last stage's, so `false | true` succeeds.

## What not to rely on

Read [bash compatibility](bash.md#not-implemented) once. The short list:
no `coproc`, no history expansion, no programmable completion, no restricted
shell, and `trap ... DEBUG` never fires.
