TOPDIR = ..

include $(TOPDIR)/Preamble.make

DEFINES = -DG_DISABLE_DEPRECATED -DLINUX
DEFINES += -DVERSION=\"$(VERSION)\"

INCLUDES = -I$(TOPDIR)/include -Iinclude
INCLUDES += $(GLIB_CFLAGS)

UNINST_PROGRAMS = fextend

CFILES = main.c

HFILES = include/main.h

DIST_FILES = $(CFILES) $(HFILES)
DIST_RULES = dist-subdircreate

OBJS = $(subst .c,.o,$(CFILES))

LIBOCFS2_LIBS = -L$(TOPDIR)/libocfs2 -locfs2
LIBOCFS2_DEPS = $(TOPDIR)/libocfs2/libocfs2.a

LIBO2DLM_LIBS = -L$(TOPDIR)/libo2dlm -lo2dlm $(DL_LIBS)
LIBO2DLM_DEPS = $(TOPDIR)/libo2dlm/libo2dlm.a

ifneq ($(BUILD_FSDLM_SUPPORT),)
LIBO2CB_LIBS = -L$(TOPDIR)/libo2cb -lo2cb -ldlm_lt
else
LIBO2CB_LIBS = -L$(TOPDIR)/libo2cb -lo2cb
endif
LIBO2CB_DEPS = $(TOPDIR)/libo2cb/libo2cb.a

dist-subdircreate:
	$(TOPDIR)/mkinstalldirs $(DIST_DIR)/include

fextend: $(OBJS) $(LIBOCFS2_DEPS) $(LIBO2DLM_DEPS) $(LIBO2CB_DEPS)
	$(LINK) $(LIBOCFS2_LIBS) $(LIBO2DLM_LIBS) $(LIBO2CB_LIBS) $(GLIB_LIBS) $(COM_ERR_LIBS) $(AIO_LIBS) -lm

include $(TOPDIR)/Postamble.make
