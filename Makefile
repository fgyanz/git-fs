all: git-fs.c
	gcc git-fs.c tree.c inode.c -o git-fs -lfuse3 -lgit2

passthrough:
	sudo setcap cap_sys_admin+ep ./git-fs
