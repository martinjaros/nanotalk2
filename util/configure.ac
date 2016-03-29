AC_INIT([nanotalk-server], [2.0])
AM_INIT_AUTOMAKE([foreign no-define])
AM_SILENT_RULES([yes])

AC_PROG_CC([gcc])

SODIUM_REQUIRED=1.0.3
PKG_CHECK_MODULES(SODIUM, [libsodium >= $SODIUM_REQUIRED],
[AC_SUBST(SODIUM_CFLAGS) AC_SUBST(SODIUM_LIBS)], [AC_MSG_ERROR([
    You need to install or upgrade the Sodium development package.
    On debian-based systems this is:
        libsodium-dev >= $SODIUM_REQUIRED
])])

GLIB_REQUIRED=2.40
PKG_CHECK_MODULES(GLIB, [glib-2.0 >= $GLIB_REQUIRED gio-2.0 >= $GLIB_REQUIRED],
[AC_SUBST(GLIB_CFLAGS) AC_SUBST(GLIB_LIBS)], [AC_MSG_ERROR([
    You need to install or upgrade the GLib development package.
    On debian-based systems this is:
        libglib-2.0-dev >= $GTK_REQUIRED
])])

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([Makefile])
AC_OUTPUT
