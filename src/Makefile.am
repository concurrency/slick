## Process with automake to produce Makefile.in

@SET_MAKE@
AUTOMAKE_OPTIONS = foreign

lib_LIBRARIES = libslick.a

CFLAGS = @CFLAGS@ -Wall -fomit-frame-pointer -D _GNU_SOURCE -O0
LDFLAGS = @LDFLAGS@

libslick_a_SOURCES = sutil.c sutil.h slick.h slick_types.h sched.c slick.c slickasm.s

