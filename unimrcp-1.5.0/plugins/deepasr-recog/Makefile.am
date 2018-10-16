AM_CPPFLAGS                = $(UNIMRCP_PLUGIN_INCLUDES)

plugin_LTLIBRARIES         = deepasrrecog.la

deepasrrecog_la_SOURCES       = src/deepasr_recog_engine.c
deepasrrecog_la_LDFLAGS       = $(UNIMRCP_PLUGIN_OPTS) \
                              -L$(top_srcdir)/plugins/third-party/deepasr/libs/ \
                              -ldl -lpthread -lrt -lDeepASR
deepasrrecog_ladir            = $(libdir)
deepasrrecog_la_DATA          = $(top_srcdir)/plugins/third-party/deepasr/libs/libDeepASR.so

include $(top_srcdir)/build/rules/uniplugin.am

UNIMRCP_PLUGIN_INCLUDES     += -I$(top_srcdir)/plugins/third-party/deepasr/include

