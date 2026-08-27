# The Tribios preview formula.
#
# It builds from the immutable release archive documented in
# docs/release-and-install-contract.md and installs through the upstream
# `cmake --install` rules, so the layout here is the same layout the packaging
# smoke test asserts.
#
# The url and sha256 lines are rewritten by packaging/homebrew/update-formula.sh
# from the checksum file published beside the archive. Do not edit them by hand.
class TribiosVfs < Formula
  desc "Native copy-on-write workspaces for parallel coding agents"
  homepage "https://github.com/JediNakDev/tribios-vfs"
  url "https://github.com/JediNakDev/tribios-vfs/releases/download/v0.0.1/tribios-vfs-0.0.1.tar.gz"
  sha256 "0000000000000000000000000000000000000000000000000000000000000000"
  license "MIT"

  depends_on "cmake" => :build
  depends_on "ninja" => :build
  depends_on "pkgconf" => :build
  depends_on "sqlite"

  depends_on :macos

  def install
    # TRIBIOS_BUILD_TESTS=OFF keeps the Catch2 fetch out of the build, so the
    # formula needs no network access after Homebrew has the archive.
    system "cmake", "-S", ".", "-B", "build", "-G", "Ninja",
           "-DTRIBIOS_BUILD_TESTS=OFF", *std_cmake_args
    system "cmake", "--build", "build"
    system "cmake", "--install", "build"
  end

  def caveats
    <<~EOS
      Tribios is a preview at 0.y.z. Anything may change between minor versions.
    EOS
  end

  test do
    assert_match "tribios #{version}", shell_output("#{bin}/tribios version")

    # The core workflow: configure a Project, run the daemon, write and read
    # back through a native APFS Workspace.
    project = testpath/"project"
    (project/"docs").mkpath
    (project/"docs/notes.txt").write "base content\n"
    system "git", "-C", project, "init", "--quiet", "--initial-branch=main"
    system "git", "-C", project, "config", "user.email", "tribios@example.invalid"
    system "git", "-C", project, "config", "user.name", "Tribios Formula Test"
    system "git", "-C", project, "add", "-A"
    system "git", "-C", project, "commit", "--quiet", "-m", "initial commit"

    system bin/"tribios", "configure", project
    system bin/"tribios", "--project", project, "daemon", "start"
    begin
      system bin/"tribios", "--project", project, "workspace", "create", "packaged"
      assert_match "packaged",
                   shell_output("#{bin}/tribios --project #{project} workspace list")
      workspace = project/".tribios/mnt/packaged"
      (workspace/"docs/formula.txt").write "brew test works"
      assert_equal "brew test works", (workspace/"docs/formula.txt").read
      assert_equal "base content\n", (workspace/"docs/notes.txt").read
    ensure
      system bin/"tribios", "--project", project, "workspace", "remove", "packaged"
      system bin/"tribios", "--project", project, "workspace", "wait-reclaim"
      system bin/"tribios", "--project", project, "daemon", "stop"
    end
  end
end
