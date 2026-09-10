# Installing

CriSH is a single universal binary. There is nothing to configure and nothing
else to install.

## The one-liner

```sh
sh -c "$(curl -fsSL https://raw.githubusercontent.com/LearNivram/CriSH/master/install.sh)"
```

It works out where to put the binary (`/usr/local/bin` when that is writable,
`~/.local/bin` otherwise), downloads the newest release, checks its SHA-256
against the `checksums.txt` published with the release, and stops if they do
not match. It changes nothing else: no `PATH` edits, no shell configuration.

Options:

```sh
... install.sh)" -- --prefix ~/.local     # choose where it goes
... install.sh)" -- --version v0.1.0      # pin a release
... install.sh)" -- --register            # add it to /etc/shells
... install.sh)" -- --uninstall           # remove it again
```

## By hand

```sh
curl -fLO https://github.com/LearNivram/CriSH/releases/latest/download/crish-macos-universal
chmod +x crish-macos-universal
sudo mv crish-macos-universal /usr/local/bin/crish
```

[Verify the download](verifying.md) before you run it.

## Homebrew

```sh
brew tap LearNivram/crish
brew install crish
```

The formula lives in [`Formula/crish.rb`](../Formula/crish.rb) in this
repository and in the `homebrew-crish` tap.

## From source

```sh
git clone https://github.com/LearNivram/CriSH
cd CriSH
make            # this architecture only
./build.sh      # universal: arm64 and x86_64
sudo make install
```

Nothing but a C compiler is needed. See [building](building.md).

## Gatekeeper

A binary downloaded with `curl` is not quarantined, so it runs. One downloaded
with a browser is, and macOS will refuse the first run. Clear the flag:

```sh
xattr -d com.apple.quarantine /usr/local/bin/crish
```

Releases are not yet notarised. When they are, this step goes away, and
[verifying a download](verifying.md) will say so.

## Making it your login shell

You do not have to, and for most people it is not the point — CriSH is most
useful as a script runner. If you do want it:

```sh
sudo sh -c 'echo /usr/local/bin/crish >> /etc/shells'
chsh -s /usr/local/bin/crish
```

Read [bash compatibility](bash.md#not-implemented) first: the interactive layer
is deliberately small.

## Using it for scripts only

The least invasive way to use CriSH is not to install it as a shell at all.
Put it on your `PATH` and change one line:

```sh
#!/usr/bin/env crish
```

or run an existing script without touching it:

```sh
crish ./deploy.sh --dry-run
```

## Removing it

```sh
sh -c "$(curl -fsSL https://raw.githubusercontent.com/LearNivram/CriSH/master/install.sh)" -- --uninstall
```

or just `rm` the binary. CriSH writes only `~/.crish_history`, and only when
you use it interactively.
