sbin_PROGRAMS = charon-cmd

charon_cmd_SOURCES = \
	cmd/cmd_options.h cmd/cmd_options.c \
	cmd/cmd_connection.h cmd/cmd_connection.c \
	cmd/cmd_creds.h cmd/cmd_creds.c \
	charon-cmd.c

charon-cmd.o :	$(top_builddir)/config.status

INCLUDES = \
	-I$(top_srcdir)/src/libstrongswan \
	-I$(top_srcdir)/src/libhydra \
	-I$(top_srcdir)/src/libcharon

AM_CFLAGS = \
	-DIPSEC_DIR=\"${ipsecdir}\" \
	-DIPSEC_PIDDIR=\"${piddir}\" \
	-DPLUGINS=\""${cmd_plugins}\""

charon_cmd_LDADD = \
	$(top_builddir)/src/libstrongswan/libstrongswan.la \
	$(top_builddir)/src/libhydra/libhydra.la \
	$(top_builddir)/src/libcharon/libcharon.la \
	-lm $(PTHREADLIB) $(DLLIB)
