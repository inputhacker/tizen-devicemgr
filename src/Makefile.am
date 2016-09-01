MAINTAINERCLEANFILES = Makefile.in
MODULE = e-mod-tizen-devicemgr

LDFLAGS +=

pkgdir                 = $(libdir)/enlightenment/modules/$(MODULE)/$(MODULE_ARCH)
pkg_LTLIBRARIES        = module.la
module_la_SOURCES      = \
	e_mod_main.c \
	e_devicemgr_output.c \
	e_devicemgr_input.c \
	e_devicemgr_scale.c \
	e_devicemgr_conf.c

if HAVE_WAYLAND_ONLY
module_la_SOURCES      += \
	e_devicemgr_dpms.c \
	e_devicemgr_screenshooter.c \
	e_devicemgr_video.c \
	e_devicemgr_viewport.c \
	e_devicemgr_tdm.c \
	e_devicemgr_buffer.c \
	e_devicemgr_embedded_compositor.c \
	e_devicemgr_device.c
endif

if HAVE_EOM
module_la_SOURCES      += \
	e_devicemgr_eom.c  eom-protocol.c
endif

module_la_LIBADD       =
module_la_CFLAGS       = @ENLIGHTENMENT_CFLAGS@ @TTRACE_CFLAGS@ @CYNARA_CFLAGS@
module_la_LDFLAGS      = -module -avoid-version @ENLIGHTENMENT_LIBS@ @TTRACE_LIBS@ @CYNARA_LIBS@
module_la_DEPENDENCIES = $(top_builddir)/config.h

if HAVE_EOM
%-protocol.c : $(top_srcdir)/protocol/%.xml
	$(AM_V_GEN)$(MKDIR_P) $(dir $@) && $(wayland_scanner) code < $< > $@

%-server-protocol.h : $(top_srcdir)/protocol/%.xml
	$(AM_V_GEN)$(MKDIR_P) $(dir $@) && $(wayland_scanner) server-header < $< > $@

%-client-protocol.h : $(top_srcdir)/protocol/%.xml
	$(AM_V_GEN)$(MKDIR_P) $(dir $@) && $(wayland_scanner) client-header < $< > $@

BUILT_SOURCES = eom-protocol.c \
                eom-client-protocol.h \
                eom-server-protocol.h

CLEANFILES = $(BUILT_SOURCES)
endif

#uninstall:
#  rm -rf $(DESTDIR)$(libdir)/enlightenment/modules/$(MODULE)
