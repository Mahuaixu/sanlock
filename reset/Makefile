TARGET1 = sanlk-resetd
TARGET2 = sanlk-reset

SOURCE1 = sanlk_resetd.c
SOURCE2 = sanlk_reset.c

OPTIMIZE_FLAG = -O2 -Wp,-D_FORTIFY_SOURCE=2
ifeq ($(DEBUG), 1)
   OPTIMIZE_FLAG = -O0
   CFLAGS += -g
endif

CFLAGS += -D_GNU_SOURCE \
	-Wall \
	-Wformat \
	-Wformat-security \
	-Wnested-externs \
	-Wpointer-arith \
	-Wextra -Wshadow \
	-Wcast-align \
	-Wwrite-strings \
	-Waggregate-return \
	-Wstrict-prototypes \
	-Winline \
	-Wredundant-decls \
	-Wno-sign-compare \
	-fexceptions \
	-fasynchronous-unwind-tables \
	-fdiagnostics-show-option \
	$(OPTIMIZE_FLAG) \
	$(NULL)

VER=$(shell cat ../VERSION)
CFLAGS += -DVERSION=\"$(VER)\" -I../src -I../wdmd
CFLAGS += -fPIE -DPIE

LDFLAGS = -Wl,-z,now -Wl,-z,relro -pie
LDADD = -lsanlock -lwdmd

all: $(TARGET1) $(TARGET2)

$(TARGET1): $(SOURCE1)
	$(CC) $(CFLAGS) $(LDFLAGS) $(SOURCE1) $(LDADD) -o $@ -L. -L../src -L../wdmd

$(TARGET2): $(SOURCE2)
	$(CC) $(CFLAGS) $(LDFLAGS) $(SOURCE2) $(LDADD) -o $@ -L. -L../src -L../wdmd

clean:
	rm -f *.o *.so *.so.* $(TARGET1) $(TARGET2)

INSTALL=$(shell which install)

DESTDIR=
BINDIR=/usr/sbin
LIBDIR=/usr/lib64
HEADIR=/usr/include
MANDIR=/usr/share/man

.PHONY: install
install: all
	$(INSTALL) -d $(DESTDIR)/$(BINDIR)
	$(INSTALL) -c -m 755 $(TARGET1) $(TARGET2) $(DESTDIR)/$(BINDIR)
	$(INSTALL) -m 644 sanlk-reset.8 $(DESTDIR)/$(MANDIR)/man8/
	$(INSTALL) -m 644 sanlk-resetd.8 $(DESTDIR)/$(MANDIR)/man8/
