# frozen_string_literal: true

# CriSH - a shell for macOS that runs Linux scripts unchanged.
class Crish < Formula
  desc "Shell that runs Linux scripts on macOS: bash 5 language, GNU utilities built in"
  homepage "https://github.com/LearNivram/CriSH"
  url "https://github.com/LearNivram/CriSH/archive/refs/tags/v0.2.0.tar.gz"
  sha256 "4e495e54b8ee38d35795add5985f4392c0ac2ce9f1dad0ed121cc073b3aaccb9"
  license "GPL-3.0-or-later"
  head "https://github.com/LearNivram/CriSH.git", branch: "master"

  depends_on :macos

  def install
    system "./build.sh"
    bin.install "build/crish"
    doc.install "README.md", "docs"
  end

  def caveats
    <<~EOS
      CriSH is most useful as a script runner:

        crish ./your-linux-script.sh
        #!/usr/bin/env crish

      To make it a login shell instead:

        echo #{opt_bin}/crish | sudo tee -a /etc/shells
        chsh -s #{opt_bin}/crish

      Read #{opt_bin}/../share/doc/crish/docs/bash.md first: the interactive
      layer is deliberately small.
    EOS
  end

  test do
    assert_match "CriSH", shell_output("#{bin}/crish --version")
    # the things a stock macOS cannot do
    assert_equal "v", shell_output("#{bin}/crish -c 'declare -A m; m[k]=v; echo ${m[k]}'").chomp
    assert_equal "X", shell_output("#{bin}/crish -c 'v=x; echo ${v^^}'").chomp
    assert_equal "1970-01-01", shell_output("#{bin}/crish -c 'date -u -d @0 +%F'").chomp
    (testpath/"f").write "hi\n"
    system bin/"crish", "-c", "sed -i 's/hi/ho/' #{testpath}/f"
    assert_equal "ho\n", (testpath/"f").read
  end
end
