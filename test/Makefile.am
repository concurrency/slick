## Process with automake to produce Makefile.in

@SET_MAKE@
AUTOMAKE_OPTIONS = foreign

bin_PROGRAMS = commstime commstime2

commstime_SOURCES = commstime.c commstime_code.s
commstime_LDADD = @srcdir@/../src/libslick.a -lpthread

commstime2_SOURCES = commstime.c commstime2_code.s
commstime2_LDADD = @srcdir@/../src/libslick.a -lpthread

CFLAGS = @CFLAGS@ -Wall -fomit-frame-pointer -D _GNU_SOURCE -I@srcdir@/../src
LDFLAGS = @LDFLAGS@ -L@srcdir@/../src

## commstime$(EXEEXT): @srcdir@/../src/libslick.a

