#ifndef INODE_H
#define INODE_H

#include <sys/stat.h>

#define T_DIR           S_IFDIR
#define T_FILE          S_IFREG
#define nlink(x)        ((x) == T_DIR ? 2 : 1)

#define INODE_STATIC   (1u << 0)
#define INODE_DELETED  (1u << 1)
#define INODE_DETACHED (1u << 2)

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
	int (*open)(struct git_repository *, struct inode *);
};

struct inode {
	char *name;
	mode_t mode;
	unsigned type;
	unsigned long ino;
	unsigned flags;
	unsigned long nlookup;
	size_t size;
	union {
		struct inode *parent;
		unsigned long parent_ino;
	};
	struct inode *sibling;
	struct inode *child;
	struct inode *next_free;
	struct inode *retired;
	struct inode_ops *ops;
	struct git_object *obj;
	int backing_id;
};

extern struct inode_ops *get_inode_ops(unsigned);

#define aload(p)       __atomic_load_n(p, __ATOMIC_ACQUIRE)
#define astore(p, v)   __atomic_store_n(p, v, __ATOMIC_RELEASE)
#define axchg(p, v)    __atomic_exchange_n(p, v, __ATOMIC_ACQ_REL)
#define acas(p, e, d)  __atomic_compare_exchange_n(p, e, d, 1, \
                           __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)
#define afadd(p, v)    __atomic_fetch_add(p, v, __ATOMIC_ACQ_REL)
#define afsub(p, v)    __atomic_fetch_sub(p, v, __ATOMIC_ACQ_REL)
#define afor(p, v)     __atomic_fetch_or(p, v, __ATOMIC_RELEASE)

#endif
