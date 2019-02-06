Name: e-mod-tizen-devicemgr
Version: 0.2.0
Release: 1
Summary: The devicemgr for enlightenment modules
URL: http://www.enlightenment.org
Group: Graphics & UI Framework/Other
Source0: %{name}-%{version}.tar.gz
License: BSD-2-Clause
BuildRequires: pkgconfig(enlightenment)
BuildRequires: pkgconfig(dlog)
BuildRequires: pkgconfig(wayland-server)
BuildRequires: pkgconfig(tizen-extension-server)


%global TZ_SYS_RO_SHARE  %{?TZ_SYS_RO_SHARE:%TZ_SYS_RO_SHARE}%{!?TZ_SYS_RO_SHARE:/usr/share}

%description
This package is a devicemgr for enlightenment.

%prep
%setup -q

%build

export GC_SECTIONS_CFLAGS="-fdata-sections -ffunction-sections"
export GC_SECTIONS_LDFLAGS="-Wl,--gc-sections"
export CFLAGS+=" -Wall -Werror -g -fPIC ${GC_SECTIONS_CFLAGS} -DE_LOGGING=1"
export LDFLAGS+=" -Wl,--hash-style=both -Wl,--as-needed -Wl,--rpath=/usr/lib -rdynamic ${GC_SECTIONS_LDFLAGS}"

%reconfigure --enable-wayland-only

make

%install
rm -rf %{buildroot}

# install
make install DESTDIR=%{buildroot}

# clear useless textual files
find  %{buildroot}%{_libdir}/enlightenment/modules/%{name} -name *.la | xargs rm

%files
%defattr(-,root,root,-)
%{_libdir}/enlightenment/modules/e-mod-tizen-devicemgr
%license COPYING
