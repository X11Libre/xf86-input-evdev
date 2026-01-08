#  Copyright 2005 Adam Jackson.
#
#  Permission is hereby granted, free of charge, to any person obtaining a
#  copy of this software and associated documentation files (the "Software"),
#  to deal in the Software without restriction, including without limitation
#  on the rights to use, copy, modify, merge, publish, distribute, sub
#  license, and/or sell copies of the Software, and to permit persons to whom
#  the Software is furnished to do so, subject to the following conditions:
#
#  The above copyright notice and this permission notice (including the next
#  paragraph) shall be included in all copies or substantial portions of the
#  Software.
#
#  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
#  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
#  FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.  IN NO EVENT SHALL
#  ADAM JACKSON BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
#  IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
#  CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#
# Process this file with autoconf to produce a configure script

# Initialize Autoconf
AC_PREREQ([2.60])
AC_INIT([xlibre-xf86-input-evdev],
        [25.0.0],
        [https://github.com/X11Libre/xf86-input-evdev/issues],
        [xlibre-xf86-input-evdev])
AC_CONFIG_SRCDIR([Makefile.am])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_AUX_DIR(.)

# Initialize Automake
AM_INIT_AUTOMAKE([foreign dist-xz])

# Initialize libtool
AC_DISABLE_STATIC
AC_PROG_LIBTOOL

# Initialize X.Org macros 1.8 or later for MAN_SUBSTS set by XORG_MANPAGE_SECTIONS
m4_ifndef([XORG_MACROS_VERSION], 
          [m4_fatal([must install xorg-macros 1.8 or later before running autoconf/autogen])])
XORG_MACROS_VERSION(1.8)
XORG_DEFAULT_OPTIONS

# Obtain compiler/linker options from server and required extensions
PKG_CHECK_MODULES(XORG, [xorg-server >= 25.0.0] xproto [inputproto >= 2.1.99.3])
PKG_CHECK_MODULES(UDEV, libudev,
    [HAVE_UDEV=yes], [HAVE_UDEV=no])
AS_IF([test "$HAVE_UDEV" = "yes"],
    [AC_DEFINE([HAVE_LIBUDEV], [1], [Define if libudev is available])])

PKG_CHECK_MODULES(LIBEVDEV, [libevdev >= 0.4])
PKG_CHECK_MODULES(MTDEV, mtdev)

# Define a configure option for an alternate input module directory
PKG_PROG_PKG_CONFIG([0.25])
moduledir=`$PKG_CONFIG --variable=moduledir xorg-server`
inputdir=${moduledir}/input
AC_SUBST(inputdir)

xorgconfdir=`$PKG_CONFIG --variable=sysconfigdir xorg-server`
AC_SUBST(xorgconfdir)

AC_OUTPUT([Makefile src/Makefile man/Makefile man/evdev.man])
