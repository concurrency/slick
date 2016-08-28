dnl -*- shell-script -*-
dnl
dnl Process this file with autoconf to produce a configure scripts.
dnl

AC_REVISION($Rev$)
AC_INIT([slick], [0.0.1], [frmb@kent.ac.uk])
AC_PREREQ(2.59)

AC_CONFIG_HEADERS([config.h])
AC_CANONICAL_SYSTEM

AC_GNU_SOURCE

AM_INIT_AUTOMAKE

m4_pattern_allow([^AS_DEFS$])

dnl Checks for programs.
AC_LANG([C])
AC_PROG_CC
AM_PROG_AS
AC_PROG_RANLIB
AC_PROG_LN_S
AM_PROG_CC_STDC
AC_PROG_MAKE_SET

AC_CHECK_TOOL(AS,as)
AC_CHECK_TOOL(LD,ld)

dnl AC_CHECK_PROG(GPERF,gperf,gperf)

dnl Internationalization
AM_GNU_GETTEXT

dnl AM_C_PROTOTYPES

AC_HEADER_STDC
AC_CHECK_HEADERS(unistd.h stdlib.h string.h stdarg.h stdint.h sys/types.h fcntl.h malloc.h sys/mman.h time.h)

dnl Checks for libraries.
AC_CHECK_LIB(pthread, pthread_create, have_libpthread=yes, have_libpthread=no)


AC_ARG_ENABLE([debug], AS_HELP_STRING([--enable-debug], [enable debugging messages (default disabled)]), [enable_debug=$enableval], [enable_debug=no])
AC_MSG_CHECKING(whether to enable debugging messages)
AC_MSG_RESULT($enable_debug)
if test "$enable_debug" = yes; then
 AC_DEFINE([SLICK_DEBUG],1,[define to enable debugging messages from the scheduler])
fi

AC_ARG_ENABLE([paranoid], AS_HELP_STRING([--enable-paranoid], [enable in-scheduler safety checks (default disabled)]), [enable_paranoid=$enableval], [enable_paranoid=no])
AC_MSG_CHECKING(whether to enable in-scheduler safety checks)
AC_MSG_RESULT($enable_paranoid)
if test "$enable_paranoid" = yes; then
 AC_DEFINE([SLICK_PARANOID],1,[define to enable in-scheduler safety checks])
fi


dnl Borrowed from CCSP/Carl.
AC_MSG_CHECKING(support for thread-local-storage)
AC_TRY_RUN([
  __thread int bar;

  void foo (void)
  {
   bar = 42;
  }

  int main (void)
  {
   foo (); 
   return bar - 42;
  }], [tls_supported=yes], [tls_supported=no], [tls_supported=no])

if test "$tls_supported" = no; then
 AC_MSG_RESULT(no)
 AC_MSG_ERROR([no TLS support])
else
 AC_MSG_RESULT(yes)
fi


dnl Checks for header files.
dnl AC_PATH_XTRA

dnl Checks for typedefs, structures, and compiler characteristics.

dnl Checks for library functions.
AC_FUNC_VPRINTF

AC_OUTPUT([Makefile src/Makefile test/Makefile])

