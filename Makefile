all: git-fs.c
	gcc git-fs.c -o git-fs -lfuse3 -lgit2
