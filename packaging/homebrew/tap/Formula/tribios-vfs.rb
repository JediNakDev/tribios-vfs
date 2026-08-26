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
  desc "Copy-on-write virtual filesystem for parallel agent workspaces"
  homepage "https://github.com/JediNakDev/tribios-vfs"
  url "https://github.com/JediNakDev/tribios-vfs/releases/download/v0.0.1/tribios-vfs-0.0.1.tar.gz"
  sha256 "0000000000000000000000000000000000000000000000000000000000000000"
  license "MIT"

  depends_on "cmake" => :build
  depends_on "ninja" => :build
  depends_on "pkgconf" => :build
  depends_on "sqlite"

  # macFUSE is a cask, so this formula can never go to homebrew/core as written.
  # JediNakDev/tribios-vfs#20 tracks that constraint.
  depends_on cask: "macfuse"

  # Tribios speaks the FUSE 2.x API. On Linux that means libfuse 2.x, which
  # Homebrew does not package, so the tap is macOS-only for now. The tap README
  # records the Linux route: build from source against libfuse-dev.
  depends_on :macos

  def install
    # macFUSE installs outside the Homebrew prefix on every architecture, so its
    # pkg-config files are not on the default search path on Apple Silicon.
    macfuse_pkgconfig_dir = "/usr/local/lib/pkgconfig"
    unless File.exist?("#{macfuse_pkgconfig_dir}/fuse.pc") ||
           File.exist?("#{macfuse_pkgconfig_dir}/osxfuse.pc")
      # Without this, CMake quietly falls back to the stub adapter and installs
      # a tribios that cannot mount. Failing loudly is the honest outcome.
      odie "macFUSE was not found under #{macfuse_pkgconfig_dir}. " \
           "Install it with `brew install --cask macfuse` and retry."
    end
    ENV.prepend_path "PKG_CONFIG_PATH", macfuse_pkgconfig_dir

    # TRIBIOS_BUILD_TESTS=OFF keeps the Catch2 fetch out of the build, so the
    # formula needs no network access after Homebrew has the archive.
    system "cmake", "-S", ".", "-B", "build", "-G", "Ninja",
           "-DTRIBIOS_ENABLE_FUSE=ON", "-DTRIBIOS_BUILD_TESTS=OFF", *std_cmake_args
    system "cmake", "--build", "build"
    system "cmake", "--install", "build"
  end

  def caveats
    <<~EOS
      Mounting a Workspace needs the macFUSE system extension, which macOS will
      not load until you approve it:

        1. Open System Settings > Privacy & Security.
        2. Allow the system software from developer "Benjamin Fleischer".
        3. Restart the Mac. macOS only loads the extension after a reboot.

      Until then `tribios daemon start` still works, but it falls back to an
      unmounted daemon and reports "mount backend: unmounted" in `tribios info`.

      Tribios is a preview at 0.y.z. Anything may change between minor versions.
    EOS
  end

  test do
    assert_match "tribios #{version}", shell_output("#{bin}/tribios version")

    # The core workflow: configure a Project, run the daemon, write and read
    # back through a Workspace. --no-mount keeps this honest on a machine where
    # the macFUSE extension has not been approved, including a CI runner.
    project = testpath/"project"
    (project/"docs").mkpath
    (project/"docs/notes.txt").write "base content\n"
    system "git", "-C", project, "init", "--quiet", "--initial-branch=main"
    system "git", "-C", project, "config", "user.email", "tribios@example.invalid"
    system "git", "-C", project, "config", "user.name", "Tribios Formula Test"
    system "git", "-C", project, "add", "-A"
    system "git", "-C", project, "commit", "--quiet", "-m", "initial commit"

    system bin/"tribios", "configure", project
    system bin/"tribios", "--project", project, "daemon", "start", "--no-mount"
    begin
      system bin/"tribios", "--project", project, "workspace", "create", "packaged"
      assert_match "packaged",
                   shell_output("#{bin}/tribios --project #{project} workspace list")
      system bin/"tribios", "--project", project, "fs", "write",
             "packaged", "docs/formula.txt", "brew test works", "0"
      assert_equal "brew test works",
                   shell_output("#{bin}/tribios --project #{project} fs read " \
                                "packaged docs/formula.txt").chomp
      assert_equal "base content",
                   shell_output("#{bin}/tribios --project #{project} fs read " \
                                "packaged docs/notes.txt").chomp
    ensure
      system bin/"tribios", "--project", project, "daemon", "stop"
    end
  end
end
