.POSIX:

PREFIX = /usr/local
BINDIR = $(PREFIX)/bin
MANDIR = $(PREFIX)/share/man

PKG_CFLAGS = $$(pkg-config --cflags fuse3 libgit2)
PKG_LIBS   = $$(pkg-config --libs fuse3 libgit2)

CC      = cc
CFLAGS  = -D_GNU_SOURCE -D_FORTIFY_SOURCE=2 -Wall -Wextra \
          -std=c99 -pedantic -Wno-unused-parameter \
          -O2 -fPIE -fstack-protector-strong $(PKG_CFLAGS)
LDFLAGS = -pie -Wl,-z,relro,-z,now
LIBS    = $(PKG_LIBS)

BIN  = git-fs
OBJS = git-fs.o tree.o inode.o inotify.o

all: $(BIN)

.c.o:
	$(CC) $(CFLAGS) -c $<

git-fs.o:  git-fs.c inode.h tree.h inotify.h
tree.o:    tree.c inode.h tree.h
inode.o:   inode.c inode.h tree.h
inotify.o: inotify.c inode.h tree.h inotify.h

$(BIN): $(OBJS)
	$(CC) $(LDFLAGS) -o $(BIN) $(OBJS) $(LIBS)

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -s -m 755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	install -d $(DESTDIR)$(MANDIR)/man1
	install -m 644 $(BIN).1 $(DESTDIR)$(MANDIR)/man1/$(BIN).1

passthrough: install
	setcap cap_sys_admin+ep $(DESTDIR)$(BINDIR)/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	rm -f $(DESTDIR)$(MANDIR)/man1/$(BIN).1

TESTS = tests/test_tree tests/test_inode

test: $(TESTS) $(BIN)
	./tests/test_tree
	./tests/test_inode
	./tests/test_mount.sh

stress: $(BIN)
	REPO=$(REPO) ./tests/test_stress.sh

stress-kernel: $(BIN)
	./tests/test_kernel_stress.sh

tests/test_tree: tests/test_tree.c tree.c inode.c inode.h tree.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_tree.c tree.c inode.c $(LIBS)

tests/test_inode: tests/test_inode.c tree.c inode.c inode.h tree.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_inode.c tree.c inode.c $(LIBS)

clean:
	rm -f $(BIN) $(OBJS) $(TESTS)
