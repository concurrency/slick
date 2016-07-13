## Process with automake to produce Makefile.in

@SET_MAKE@
AUTOMAKE_OPTIONS = foreign

bin_PROGRAMS = commstime

commstime_SOURCES = commstime.c commstime_code.s
commstime_LDADD = -lslick -lpthread

CFLAGS = @CFLAGS@ -Wall -fomit-frame-pointer -D _GNU_SOURCE -I@srcdir@/../src
LDFLAGS = @LDFLAGS@ -L@srcdir@/../src

## commstime$(EXEEXT): @srcdir@/../src/libslick.a

