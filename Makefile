# ce Makefile

BIN=ce
CC?=cc
PREFIX?=/usr/local
OBJDIR?=obj
INSTALL_DIR=$(PREFIX)/bin
MAN_DIR?=$(PREFIX)/share/man

SRC=	ce.c buffer.c dirlist.c editor.c proc.c syntax.c term.c utf8.c

CFLAGS+=-Wall -Werror -Wstrict-prototypes -Wmissing-prototypes
CFLAGS+=-Wmissing-declarations -Wshadow -Wpointer-arith -Wcast-qual
CFLAGS+=-Wsign-compare -std=c99 -pedantic -ggdb
CFLAGS+=-DPREFIX='"$(PREFIX)"' -fstack-protector-all

OBJS=	$(SRC:%.c=$(OBJDIR)/%.o)

OSNAME=$(shell uname -s | sed -e 's/[-_].*//g' | tr A-Z a-z)
ifeq ("$(OSNAME)", "darwin")
	OBJS+=$(OBJDIR)/macos.o
	LDFLAGS+=-framework Foundation -framework Appkit
else ifeq ("$(OSNAME)", "linux")
	CFLAGS+=-D_GNU_SOURCE
endif

all: $(BIN)

$(BIN): $(OBJDIR) $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $(BIN)

$(OBJDIR):
	@mkdir -p $(OBJDIR)

install:
	mkdir -p $(INSTALL_DIR)
	install -m 555 $(BIN) $(INSTALL_DIR)/$(BIN)

$(OBJDIR)/%.o: %.m
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BIN) $(OBJDIR)

.PHONY: all clean
