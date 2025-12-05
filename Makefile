all: git-fs.c
	gcc git-fs.c tree.c inode.c -o git-fs -lfuse3 -lgit2
