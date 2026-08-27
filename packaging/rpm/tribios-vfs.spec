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
Summary:        Native copy-on-write Workspaces for Git projects

License:        MIT
URL:            https://github.com/JediNakDev/tribios-vfs
Source0:        %{url}/releases/download/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.24
BuildRequires:  ninja-build
BuildRequires:  pkgconfig
BuildRequires:  pkgconfig(sqlite3)
BuildRequires:  systemd-rpm-macros
# std::expected needs libstdc++ 12 or newer. RHEL 9's system GCC is 11, so the
# EPEL 9 build uses a toolset compiler. Fedora and EPEL 10 are new enough.
%if 0%{?rhel} == 9
BuildRequires:  gcc-toolset-14-gcc-c++
%else
BuildRequires:  gcc-c++ >= 12
%endif

Requires:       btrfs-progs
Requires:       git-core

%description
Tribios VFS gives a Git project native copy-on-write Workspaces.
It uses Btrfs snapshots where available and kernel OverlayFS otherwise.
One Workspace never disturbs the immutable Base state or its siblings.

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
%cmake -GNinja -DTRIBIOS_BUILD_TESTS=OFF
%cmake_build

%install
%cmake_install

%post
%systemd_post tribios-storage.service

%preun
%systemd_preun tribios-storage.service

%postun
%systemd_postun_with_restart tribios-storage.service

%check
test "$(%{buildroot}%{_bindir}/tribios version)" = "tribios %{version}"

%files
%{_bindir}/tribios
%dir %{_libexecdir}/tribios
%{_libexecdir}/tribios/tribios_daemon
%{_libexecdir}/tribios/tribios_storage_service
%{_unitdir}/tribios-storage.service
%dir %{_datadir}/doc/%{name}
%license %{_datadir}/doc/%{name}/LICENSE
%doc %{_datadir}/doc/%{name}/README.md

%changelog
* Wed Aug 26 2026 Pitchayut Ariyachansil <pitch.jedi@gmail.com> - 0.0.1-1
- First Copr preview build
