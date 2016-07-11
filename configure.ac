AC_INIT([nanotalk], [2.1.0])

AM_INIT_AUTOMAKE
AC_PROG_CC([gcc])

AM_GNU_GETTEXT([external])
AM_GNU_GETTEXT_VERSION([0.18.3])

AC_ARG_ENABLE([gui], AS_HELP_STRING([--disable-gui], [build without GUI]))
AM_CONDITIONAL(ENABLE_GUI, [test "x$enable_gui" != "xno"])

SODIUM_REQUIRED=1.0.8
GLIB_REQUIRED=2.40
GTK_REQUIRED=3.10
GST_REQUIRED=1.2
CANBERRA_REQUIRED=0.30

PKG_CHECK_MODULES(SODIUM, [libsodium >= $SODIUM_REQUIRED],
[AC_SUBST(SODIUM_CFLAGS) AC_SUBST(SODIUM_LIBS)], [AC_MSG_ERROR([
    You need to install or upgrade the Sodium development package.
    On debian-based systems this is:
        libsodium-dev >= $SODIUM_REQUIRED
])])

PKG_CHECK_MODULES(GLIB, [glib-2.0 >= $GLIB_REQUIRED gio-2.0 >= $GLIB_REQUIRED],
[AC_SUBST(GLIB_CFLAGS) AC_SUBST(GLIB_LIBS)], [AC_MSG_ERROR([
    You need to install or upgrade the GLib development package.
    On debian-based systems this is:
        libglib2.0-dev >= $GTK_REQUIRED
])])

if test "x$enable_gui" != "xno"; then
  AC_DEFINE(ENABLE_GUI, 1, [Build GTK+ frontend.])

  PKG_CHECK_MODULES(GTK, [gtk+-3.0 >= $GTK_REQUIRED],
  [AC_SUBST(GTK_CFLAGS) AC_SUBST(GTK_LIBS)], [AC_MSG_ERROR([
    You need to install or upgrade the GTK+ development package.
    On debian-based systems this is:
        libgtk-3-dev >= $GTK_REQUIRED
  ])])

  PKG_CHECK_MODULES(GST, [gstreamer-1.0 >= $GST_REQUIRED gstreamer-base-1.0 >= $GST_REQUIRED],
  [AC_SUBST(GST_CFLAGS) AC_SUBST(GST_LIBS)], [AC_MSG_ERROR([
    You need to install or upgrade the GStreamer development packages.
    On debian-based systems these are:
        libgstreamer1.0-dev >= $GST_REQUIRED
        libgstreamer-plugins-base1.0-dev >= $GST_REQUIRED
  ])])

  PKG_CHECK_MODULES(CANBERRA, [libcanberra >= $CANBERRA_REQUIRED],
  [AC_DEFINE(HAVE_CANBERRA, [1], [Enable libcanberra support.]) AC_SUBST(CANBERRA_CFLAGS) AC_SUBST(CANBERRA_LIBS)], [:])
fi

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([Makefile src/Makefile po/Makefile.in])
AC_OUTPUT
