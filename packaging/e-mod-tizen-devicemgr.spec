Name: e-mod-tizen-devicemgr
Version: 0.1.39
Release: 2
Summary: The devicemgr for enlightenment modules
URL: http://www.enlightenment.org
Group: Graphics & UI Framework/Other
Source0: %{name}-%{version}.tar.gz
License: BSD-2-Clause
BuildRequires: pkgconfig(enlightenment)
BuildRequires: pkgconfig(dlog)
BuildRequires: pkgconfig(ttrace)
BuildRequires: pkgconfig(libtbm)
BuildRequires: pkgconfig(libtdm)
BuildRequires: pkgconfig(libpng)
BuildRequires: pkgconfig(pixman-1)
BuildRequires: pkgconfig(wayland-server)
BuildRequires: pkgconfig(screenshooter-server)
BuildRequires: pkgconfig(tizen-extension-server)
BuildRequires: pkgconfig(wayland-tbm-server)
BuildRequires: pkgconfig(cynara-client)
BuildRequires: pkgconfig(cynara-creds-socket)
%if "%{?profile}" == "mobile"
BuildRequires: pkgconfig(eom)
%endif
%if "%{?profile}" == "wearable"
BuildRequires: pkgconfig(eom)
%endif

%global TZ_SYS_RO_SHARE  %{?TZ_SYS_RO_SHARE:%TZ_SYS_RO_SHARE}%{!?TZ_SYS_RO_SHARE:/usr/share}

%description
This package is a devicemgr for enlightenment.

%if "%{?profile}" == "mobile"
%define EOM use
%endif
%if "%{?profile}" == "wearable"
%define EOM use
%endif
%prep
%setup -q

%build

export GC_SECTIONS_FLAGS="-fdata-sections -ffunction-sections -Wl,--gc-sections"
export CFLAGS+=" -Wall -Werror -g -fPIC -rdynamic ${GC_SECTIONS_FLAGS} -DE_LOGGING=1"
export LDFLAGS+=" -Wl,--hash-style=both -Wl,--as-needed -Wl,--rpath=/usr/lib"

%if "%{EOM}" == "use"
%reconfigure --enable-wayland-only --enable-cynara --enable-eom
%else
%reconfigure --enable-wayland-only --enable-cynara
%endif

make

%install
rm -rf %{buildroot}

# for license notification
mkdir -p %{buildroot}/%{TZ_SYS_RO_SHARE}/license
cp -a %{_builddir}/%{buildsubdir}/COPYING %{buildroot}/%{TZ_SYS_RO_SHARE}/license/%{name}

# install
make install DESTDIR=%{buildroot}

# clear useless textual files
find  %{buildroot}%{_libdir}/enlightenment/modules/%{name} -name *.la | xargs rm

%files
%defattr(-,root,root,-)
%{_libdir}/enlightenment/modules/e-mod-tizen-devicemgr
%{TZ_SYS_RO_SHARE}/license/%{name}
