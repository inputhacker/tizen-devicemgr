MAINTAINERCLEANFILES = Makefile.in
MODULE = e-mod-tizen-devicemgr

LDFLAGS +=

pkgdir                 = $(libdir)/enlightenment/modules/$(MODULE)/$(MODULE_ARCH)
pkg_LTLIBRARIES        = module.la
module_la_SOURCES      = \
	e_mod_main.c \
	e_devicemgr_input.c \
	e_devicemgr_conf.c

if HAVE_WAYLAND_ONLY
module_la_SOURCES      += \
	e_devicemgr_embedded_compositor.c \
	e_devicemgr_device.c
endif

module_la_LIBADD       =
module_la_CFLAGS       = @ENLIGHTENMENT_CFLAGS@ @TTRACE_CFLAGS@ @CYNARA_CFLAGS@
module_la_LDFLAGS      = -module -avoid-version @ENLIGHTENMENT_LIBS@ @TTRACE_LIBS@ @CYNARA_LIBS@
module_la_DEPENDENCIES = $(top_builddir)/config.h

#uninstall:
#  rm -rf $(DESTDIR)$(libdir)/enlightenment/modules/$(MODULE)
