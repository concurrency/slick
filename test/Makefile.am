## Process with automake to produce Makefile.in

@SET_MAKE@
AUTOMAKE_OPTIONS = foreign

bin_PROGRAMS = commstime commstime2 commstime3 procring

commstime_SOURCES = commstime.c commstime_code.s
commstime_LDADD = @srcdir@/../src/libslick.a -lpthread

commstime2_SOURCES = commstime.c commstime2_code.s
commstime2_LDADD = @srcdir@/../src/libslick.a -lpthread

commstime3_SOURCES = commstime.c commstime3_code.s
commstime3_LDADD = @srcdir@/../src/libslick.a -lpthread

procring_SOURCES = procring.c procring_code.S
procring_LDADD = @srcdir@/../src/libslick.a -lpthread

CFLAGS = @CFLAGS@ -Wall -fomit-frame-pointer -D _GNU_SOURCE -I@srcdir@/../src
LDFLAGS = @LDFLAGS@ -L@srcdir@/../src


