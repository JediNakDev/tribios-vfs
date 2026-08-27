# Fedora and EPEL spec for the Copr preview repository.
# docs/packaging/copr.md records how the Copr project is wired up and which
# chroots are claimed. docs/release-and-install-contract.md is the upstream
# contract this spec builds against: it installs exactly four files and never
# touches Project data under .tribios.
#
# Prerelease versions use RPM's tilde ordering, so a beta is spelled
#   Version: 0.2.0~beta1
# which sorts before 0.2.0. Never spell it 0.2.0-beta1; RPM sorts that after.

Name:           tribios-vfs
Version:        0.0.1
Release:        1%{?dist}
Summary:        Copy-on-write Workspace filesystem for Git projects

License:        MIT
URL:            https://github.com/JediNakDev/tribios-vfs
Source0:        %{url}/releases/download/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.24
BuildRequires:  ninja-build
BuildRequires:  pkgconfig
BuildRequires:  pkgconfig(sqlite3)
BuildRequires:  pkgconfig(fuse3)
# std::expected needs libstdc++ 12 or newer. RHEL 9's system GCC is 11, so the
# EPEL 9 build uses a toolset compiler. Fedora and EPEL 10 are new enough.
%if 0%{?rhel} == 9
BuildRequires:  gcc-toolset-14-gcc-c++
%else
BuildRequires:  gcc-c++ >= 12
%endif

# The daemon mounts through libfuse3 and unmounts by running fusermount3,
# and Workspaces are Git worktrees driven by the git command line.
Requires:       fuse3
Requires:       git-core

%description
Tribios VFS gives a Git project copy-on-write Workspaces. Each Workspace is a
mounted view of an immutable Base state: reads fall through to the Base, the
first write copies the whole file into the Workspace, and deletions are
tombstones, so one Workspace never disturbs the Base state or its siblings.

The daemon is per-project and is started by the user with `tribios daemon
start`. This package installs no system service and configures no project.

%prep
%autosetup -n %{name}-%{version}

%build
%if 0%{?rhel} == 9
. /opt/rh/gcc-toolset-14/enable
%endif
# The test tier fetches Catch2 over the network at configure time, which a
# package build cannot do.
%cmake -GNinja -DTRIBIOS_BUILD_TESTS=OFF -DTRIBIOS_ENABLE_FUSE=ON
%cmake_build

# Configure only warns when libfuse3 is not found and falls back to a
# stub with no mount support. A package that silently lost mounting would still
# build and still pass a version check, so fail the build here instead.
grep -q TRIBIOS_HAVE_FUSE %{_vpath_builddir}/compile_commands.json

%install
%cmake_install

%check
test "$(%{buildroot}%{_bindir}/tribios version)" = "tribios %{version}"

%files
%{_bindir}/tribios
%dir %{_libexecdir}/tribios
%{_libexecdir}/tribios/tribios_daemon
%dir %{_datadir}/doc/%{name}
%license %{_datadir}/doc/%{name}/LICENSE
%doc %{_datadir}/doc/%{name}/README.md

%changelog
* Wed Aug 26 2026 Pitchayut Ariyachansil <pitch.jedi@gmail.com> - 0.0.1-1
- First Copr preview build
