#ifndef INODE_H
#define INODE_H

#include <sys/stat.h>

#define T_DIR           S_IFDIR
#define T_FILE          S_IFREG
#define nlink(x)        ((x) == T_DIR ? 2 : 1)

// static inodes
enum {
	ROOT = 1,
	BRANCHES,
	TAGS,
	OBJECTS,
	HEAD,
	REMOTES,
	HEADS,
	TOP_INODES,
};

enum {
	T_GENERIC,
	T_BRANCHES,
	T_TAGS,
	T_REMOTES,
	T_COMMIT,
	T_PARENT = T_COMMIT,
	T_TREE,
	T_HASH,
	T_MSG,
	T_ALL
};

struct git_repository;
struct git_object;
struct inode;

struct inode_ops {
	int (*update)(struct git_repository *, struct inode *);
	struct inode * (*lookup)(struct git_repository *, struct inode *,const char *);
//	int (*open)();
};

struct inode {
	char *name;
	mode_t mode;
	unsigned type;
	unsigned long ino;
	size_t size;
	union {
		struct inode *parent;
		unsigned long parent_ino;
	};
	struct inode *sibling;
	struct inode *child;
	struct inode *next_free;
	struct inode_ops *ops;
	struct git_object *obj;
};

extern struct inode_ops *get_inode_ops(unsigned);

#endif
