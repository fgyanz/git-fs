.POSIX:

PREFIX = /usr/local
BINDIR = $(PREFIX)/bin

PKG_CFLAGS = $$(pkg-config --cflags fuse3 libgit2)
PKG_LIBS   = $$(pkg-config --libs fuse3 libgit2)

CC     = cc
CFLAGS = -D_GNU_SOURCE -Wall -O2 $(PKG_CFLAGS)
LDFLAGS =
LIBS   = $(PKG_LIBS)

BIN  = git-fs
OBJS = git-fs.o tree.o inode.o

all: $(BIN)

.c.o:
	$(CC) $(CFLAGS) -c $<

git-fs.o: git-fs.c inode.h tree.h
tree.o:   tree.c inode.h tree.h
inode.o:  inode.c inode.h tree.h

$(BIN): $(OBJS)
	$(CC) $(LDFLAGS) -o $(BIN) $(OBJS) $(LIBS)

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)

passthrough: install
	setcap cap_sys_admin+ep $(DESTDIR)$(BINDIR)/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)

clean:
	rm -f $(BIN) $(OBJS)
