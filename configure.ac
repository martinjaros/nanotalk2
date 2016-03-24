AC_INIT([Nanotalk distributed multimedia service], [2.0], [xjaros32@stud.feec.vutbr.cz], [nanotalk], [https://github.com/martinjaros/nanotalk2])
AM_INIT_AUTOMAKE([foreign no-define])
AM_SILENT_RULES([yes])

AC_PROG_CC([gcc])
AC_CONFIG_HEADERS([config.h])

SODIUM_REQUIRED=1.0.3
PKG_CHECK_MODULES(SODIUM, [libsodium >= $SODIUM_REQUIRED],
    [AC_SUBST(SODIUM_CFLAGS) AC_SUBST(SODIUM_LIBS)], [AC_MSG_ERROR([
    You need to install or upgrade the Sodium development package.
    On debian-based systems this is:
        libsodium-dev >= $SODIUM_REQUIRED
])])

GTK_REQUIRED=3.10
PKG_CHECK_MODULES(GTK, [gtk+-3.0 >= $GTK_REQUIRED],
    [AC_SUBST(GTK_CFLAGS) AC_SUBST(GTK_LIBS)], [AC_MSG_ERROR([
    You need to install or upgrade the GTK+ development package.
    On debian-based systems this is:
        libgtk-3-dev >= $GTK_REQUIRED
])])

GST_REQUIRED=1.2
PKG_CHECK_MODULES(GST, [gstreamer-1.0 >= $GST_REQUIRED gstreamer-rtp-1.0 >= $GST_REQUIRED],
    [AC_SUBST(GST_CFLAGS) AC_SUBST(GST_LIBS)], [AC_MSG_ERROR([
    You need to install or upgrade the GStreamer development packages.
    On debian-based systems these are:
        libgstreamer1.0-dev >= $GST_REQUIRED
        libgstreamer-plugins-base1.0-dev >= $GST_REQUIRED
])])

AC_CONFIG_FILES([Makefile src/Makefile])
AC_OUTPUT
