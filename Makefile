# ce Makefile

BIN=ce
CC?=cc
PREFIX?=/usr/local
OBJDIR?=obj
INSTALL_DIR=$(PREFIX)/bin
MAN_DIR?=$(PREFIX)/share/man

SRC=	ce.c buffer.c editor.c term.c

CFLAGS+=-Wall -Werror -Wstrict-prototypes -Wmissing-prototypes
CFLAGS+=-Wmissing-declarations -Wshadow -Wpointer-arith -Wcast-qual
CFLAGS+=-Wsign-compare -std=c99 -pedantic -ggdb
CFLAGS+=-DPREFIX='"$(PREFIX)"' -fstack-protector-all

CFLAGS+=-D_GNU_SOURCE

OBJS=	$(SRC:%.c=$(OBJDIR)/%.o)

all: $(BIN)

$(BIN): $(OBJDIR) $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $(BIN)

$(OBJDIR):
	@mkdir -p $(OBJDIR)

install:
	mkdir -p $(INSTALL_DIR)
	install -m 555 $(BIN) $(INSTALL_DIR)/$(BIN)

$(OBJDIR)/%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BIN) $(OBJDIR)

.PHONY: all clean
