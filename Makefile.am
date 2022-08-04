AUTOMAKE_OPTIONS = foreign

AM_CPPFLAGS = -W  -Wall -g -std=c99 -DPREFIX="\"$(prefix)\""

# bin
bin_PROGRAMS = sysmond
# source files
sysmond_SOURCES = ini.c sysmon.c

sysconf_DATA = sysmond.conf
install-data-local:
     ([ -d $(DESTDIR)/etc/systemd/system/ ] && cp sysmond.service $(DESTDIR)/etc/systemd/system/) || true

EXTRA_DIST = ini.h sysmond.conf sysmond.service
