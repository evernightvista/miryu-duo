%define KWIN_VERSION 6.8.1

Name:           miryu-duo
Version:        45.0.0
Release:        1%{?dist}
Summary:        Duo-style fold and blur animation for KWin

License:        GPL-3.0-or-later
URL:            https://github.com/evernightvista/miryu-duo
Source0:        https://github.com/evernightvista/miryu-duo/archive/refs/heads/main.zip

BuildRequires:  cmake
BuildRequires:  gcc-c++
BuildRequires:  gettext
BuildRequires:  extra-cmake-modules
BuildRequires:  qt6-qtbase-devel
BuildRequires:  kf6-ki18n-devel
BuildRequires:  kf6-kglobalaccel-devel
BuildRequires:  kf6-kconfig-devel
BuildRequires:  libepoxy-devel
BuildRequires:  kwin-devel = %{KWIN_VERSION}

Requires:       kwin = %{KWIN_VERSION}

%description
Miryu Duo is Kwin login and logout effect plugin and inspired by iPhone Duo.

%prep
%autosetup -n %{name}-main

%build
%cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DKDE_INSTALL_PLUGINDIR=%{_libdir}/qt6/plugins \
    -DKDE_INSTALL_LOCALEDIR=%{_datadir}/locale
%cmake_build

%install
%cmake_install
%find_lang %{name}

%files -f %{name}.lang
%license LICENSES/GPL-3.0-or-later.txt
%{_libdir}/qt6/plugins/kwin/effects/plugins/org.miryugaming.duo.so

%changelog
* Sun Oct 25 2026 KairikiFedora <13278297951@sina.cn> - 45.0.0-1
- Initial Release
