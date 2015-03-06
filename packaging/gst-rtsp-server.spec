Name:       gst-rtsp-server
Summary:    Multimedia Framework Library
Version:    1.4.1
Release:    0
Group:      System/Libraries
License:    LGPLv2+
Source0:    %{name}-%{version}.tar.gz
Requires(post):  /sbin/ldconfig
Requires(postun):  /sbin/ldconfig
BuildRequires:  pkgconfig(gstreamer-1.0)
BuildRequires:  pkgconfig(gstreamer-plugins-base-1.0)

BuildRoot:  %{_tmppath}/%{name}-%{version}-build

%description

%package devel
Summary:    Multimedia Framework RTSP server library (DEV)
Group:      Development/Libraries
Requires:   %{name} = %{version}-%{release}

%description devel

%package factory
Summary:    Multimedia Framework RTSP server Library (Factory)
Group:      Development/Libraries
Requires:   %{name} = %{version}-%{release}

%description factory

%prep
%setup -q

%build

./autogen.sh

CFLAGS+=" -DEXPORT_API=\"__attribute__((visibility(\\\"default\\\")))\" "; export CFLAGS
LDFLAGS+="-Wl,--rpath=%{_prefix}/lib -Wl,--hash-style=both -Wl,--as-needed"; export LDFLAGS

# always enable sdk build. This option should go away
./configure --enable-sdk --prefix=%{_prefix} --disable-static

# Call make instruction with smp support
#make %{?jobs:-j%jobs}
make

%install
rm -rf %{buildroot}
%make_install
mkdir -p %{buildroot}/%{_datadir}/license
cp -rf %{_builddir}/%{name}-%{version}/LICENSE.LGPLv2.1 %{buildroot}%{_datadir}/license/%{name}

%clean
rm -rf %{buildroot}

%post
/sbin/ldconfig

%postun
/sbin/ldconfig

%files
%manifest gst-rtsp-server.manifest
%defattr(-,root,root,-)
%{_datadir}/license/%{name}
%{_libdir}/*.so.*

%files devel
%defattr(-,root,root,-)
%{_libdir}/*.so
%{_includedir}/gstreamer-1.0/gst/rtsp-server/rtsp-*.h
%{_includedir}/gstreamer-1.0/gst/rtsp-server/gstwfd*.h
%{_libdir}/pkgconfig/*
