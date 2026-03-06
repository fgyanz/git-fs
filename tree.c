#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <git2.h>

#include "inode.h"
#include "tree.h"

#define TREE_MAX         (1 << 20)
#define TREE_BATCH       4096
#define MAX_TREE_DEPTH   200

/* marks the last entry in the free list. 0 means "not on the list". */
#define FREE_END         ((unsigned long)-1)
#define BATCH_RECLAIM    UINT_MAX

static struct inode top[] = {
	{},
	{
		.name = "/",
		.ino = ROOT,
		.parent_ino = ROOT,
		.mode = T_DIR,
	},
	{
		.name = "branches",
		.ino = BRANCHES,
		.parent_ino = ROOT,
		.mode = T_DIR,
	},
	{
		.name = "tags",
		.ino = TAGS,
		.parent_ino = ROOT,
		.mode = T_DIR,
		.type = T_TAGS,
	},
	{
		.name = "objects",
		.ino = OBJECTS,
		.parent_ino = ROOT,
		.mode = T_DIR,
		.type = T_OBJECTS,
	},
	{
		.name = "HEAD",
		.ino = HEAD,
		.parent_ino = ROOT,
		.mode = T_DIR,
		.type = T_COMMIT,
	},
	{
		.name = "remotes",
		.ino = REMOTES,
		.parent_ino = BRANCHES,
		.mode = T_DIR,
		.type = T_REMOTES,
	},
	{
		.name = "heads",
		.ino = HEADS,
		.parent_ino = BRANCHES,
		.mode = T_DIR,
		.type = T_BRANCHES,
	},
};

struct inode *nodes;

/* free list lives outside the inode pages so we can give pages back
 * to the kernel without breaking the list. */
unsigned long *free_next;        /* next free ino, or 0 if in use */
static unsigned long ftop_ino;   /* head of free list, 0 = empty */
static unsigned *batch_nfree;    /* free count per batch */
static size_t nhwm;              /* next unused ino */
static size_t nmax;              /* pool capacity */

/* add ino to the front of the free list. */
static void
push_list(unsigned long ino)
{
	unsigned long t;

	do {
		t = __atomic_load_n(&ftop_ino, __ATOMIC_RELAXED);
		__atomic_store_n(&free_next[ino], t ? t : FREE_END,
				 __ATOMIC_RELAXED);
	} while (!__atomic_compare_exchange_n(&ftop_ino, &t, ino, 1,
			__ATOMIC_RELEASE, __ATOMIC_RELAXED));
}

/* take one slot from batch bi. returns -1 if the batch is
 * being given back to the kernel right now. */
static int
claim_batch(unsigned bi)
{
	unsigned old;

	for (;;) {
		old = aload(&batch_nfree[bi]);
		if (old == BATCH_RECLAIM)
			return -1;
		if (acas(&batch_nfree[bi], &old, old - 1))
			return 0;
	}
}

/* if every inode in a batch is free, give the pages back to the
 * kernel. they stay on the free list and get fresh pages on reuse. */
static void
try_reclaim(unsigned bi)
{
	unsigned expected;

	if (bi == 0)
		return;  /* batch 0 has static inodes */

	expected = TREE_BATCH;
	if (!acas(&batch_nfree[bi], &expected, BATCH_RECLAIM))
		return;

	madvise((char *)(nodes + (size_t)bi * TREE_BATCH),
		TREE_BATCH * sizeof(struct inode), MADV_DONTNEED);

	astore(&batch_nfree[bi], TREE_BATCH);
}

/* wipe the node and put it back on the free list. */
static void
push_free(struct inode *n)
{
	unsigned long ino = n->ino;
	unsigned bi = ino / TREE_BATCH;
	unsigned old;

	memset(n, 0, sizeof(*n));
	n->ino = ino;

	old = afadd(&batch_nfree[bi], 1);
	push_list(ino);
	if (old + 1 == TREE_BATCH)
		try_reclaim(bi);
}

/* bring a new batch of inodes into the free list. */
static void
grow(void)
{
	size_t old, end, i;

	old = aload(&nhwm);
	if (old >= nmax)
		return;

	end = old + TREE_BATCH;
	if (end > nmax)
		end = nmax;

	if (!acas(&nhwm, &old, end))
		return;

	for (i = old; i < end; i++) {
		nodes[i].ino = i;
		afadd(&batch_nfree[i / TREE_BATCH], 1);
		push_list(i);
	}
}

/* grab a free node. grows the pool if needed. */
static struct inode *
new_node(void)
{
	unsigned long ino, nx;

retry:
	for (;;) {
		ino = aload(&ftop_ino);
		if (!ino) {
			grow();
			ino = aload(&ftop_ino);
			if (!ino)
				return NULL;
		}
		nx = __atomic_load_n(&free_next[ino], __ATOMIC_RELAXED);
		if (nx == FREE_END)
			nx = 0;
		if (acas(&ftop_ino, &ino, nx))
			break;
	}

	/* still looks free to get_tree_node until we clear free_next */
	if (claim_batch(ino / TREE_BATCH)) {
		push_list(ino);
		goto retry;
	}

	astore(&free_next[ino], 0);
	nodes[ino].ino = ino;  /* page may have been wiped by reclaim */
	return nodes + ino;
}

static inline int
is_dead(struct inode *n)
{
	return aload(&n->flags) & INODE_DELETED;
}

/* add a child to p. returns existing child if name already taken. */
struct inode *
add_tree_node(struct inode *p, const char *name, unsigned type, mode_t mode)
{
	struct inode *n, *head, *nx;

	n = get_tree_child(p, name);
	if (n)
		return n;

	n = new_node();
	if (!n)
		return NULL;

	n->name = strdup(name);
	n->ops = get_inode_ops(type);
	n->mode = mode;
	n->parent = p;
	n->type = type;

	/* first child: point to itself */
	head = aload(&p->child);
	if (!head) {
		astore(&n->sibling, n);
		if (acas(&p->child, &head, n))
			return n;
	}

	/* insert after head */
	head = aload(&p->child);
	do {
		nx = aload(&head->sibling);
		astore(&n->sibling, nx);
	} while (!acas(&head->sibling, &nx, n));

	return n;
}

/* look up a live inode by number. */
struct inode *
get_tree_node(unsigned long ino)
{
	struct inode *n;

	if (ino >= aload(&nhwm))
		return NULL;

	n = nodes + ino;
	if (aload(&free_next[ino]))
		return NULL;
	if (is_dead(n))
		return NULL;

	return n;
}

/* find a live child of p by name. */
struct inode *
get_tree_child(struct inode *p, const char *name)
{
	struct inode *c, *head;

	head = aload(&p->child);
	if (!head)
		return NULL;

	c = head;
	do {
		if (!is_dead(c) && strcmp(c->name, name) == 0)
			return c;
		c = aload(&c->sibling);
	} while (c != head);

	return NULL;
}

/* skip pos live siblings forward. used by readdir. */
struct inode *
get_tree_sibling(struct inode *node, off_t pos)
{
	struct inode *s;

	s = node;
	do {
		s = aload(&s->sibling);
		if (!is_dead(s) && --pos <= 0)
			return s;
	} while (s != node);

	return s;
}

/* count live children of p. */
size_t
count_tree_children(struct inode *p)
{
	struct inode *n, *s;
	size_t c;

	n = aload(&p->child);
	if (!n)
		return 0;

	c = is_dead(n) ? 0 : 1;
	for (s = aload(&n->sibling); s != n; s = aload(&s->sibling))
		if (!is_dead(s))
			c++;

	return c;
}

/* build the path from node up to the nearest non-tree ancestor. */
void
tree_path(struct inode *node, char *opath, size_t size, struct inode **tree)
{
	char *v[MAX_TREE_DEPTH], **vstart, **vend;
	int len;

	vstart = v;
	vend = v + MAX_TREE_DEPTH;
	*vstart++ = NULL;
	while (node->parent->type == T_TREE && vstart != vend) {
		assert(node->ino != ROOT);
		*vstart++ = node->name;
		node = node->parent;
	}

	*tree = node;

	while (*--vstart) {
		len = snprintf(opath + strlen(opath), size, "/%s", *vstart);
		if (len < 0 || (size_t) len >= size)
			break;
	}
}

/* release name and git object. */
static void
clear_node(struct inode *n)
{
	struct git_object *obj;

	free(n->name);
	n->name = NULL;

	obj = axchg(&n->obj, NULL);
	if (obj)
		git_object_free(obj);
}

/* the kernel looked up this node; bump its ref count. */
void
tree_ref(struct inode *n)
{
	afadd(&n->nlookup, 1);
}

/* the kernel is done with this node. when refs hit zero, delete it.
 * if already detached from its parent, reclaim the slot too. */
void
tree_forget(struct inode *n, unsigned long nlookup)
{
	unsigned long old;
	unsigned f;

	f = aload(&n->flags);
	if (f & INODE_STATIC)
		return;

	old = afsub(&n->nlookup, nlookup);
	if (old > nlookup)
		return;

	afor(&n->flags, INODE_DELETED);
	clear_node(n);

	/* re-read: flags may have changed */
	f = aload(&n->flags);
	if (f & INODE_DETACHED)
		push_free(n);
}

/* reclaim a retired child list. nodes still held by the kernel
 * are marked detached so tree_forget cleans them up later. */
void
free_retired(struct inode *head)
{
	struct inode *c, *nx;

	if (!head)
		return;

	c = head;
	do {
		nx = c->sibling;
		if (c->flags & INODE_STATIC) {
			/* never freed */
		} else if (aload(&c->nlookup) == 0) {
			clear_node(c);
			push_free(c);
		} else {
			afor(&c->flags, INODE_DETACHED);
		}
		c = nx;
	} while (c != head);
}

/* shutdown: free everything. */
void
tree_destroy(void)
{
	size_t i, hwm;

	if (!nodes)
		return;

	hwm = aload(&nhwm);
	for (i = TOP_INODES; i < hwm; i++) {
		if (aload(&free_next[i]))
			continue;
		clear_node(nodes + i);
	}

	munmap(nodes, nmax * sizeof(struct inode));
	munmap(free_next, nmax * sizeof(*free_next));
	free(batch_nfree);
	nodes = NULL;
}

/* set up the pools, static inodes, and initial free list. */
int
tree_init(void)
{
	int i;
	unsigned long ino;
	struct inode *n;

	nmax = TREE_MAX;
	nodes = mmap(NULL, nmax * sizeof(struct inode),
		     PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (nodes == MAP_FAILED) {
		fprintf(stderr, "%s:%s\n", __func__, strerror(errno));
		return 1;
	}

	free_next = mmap(NULL, nmax * sizeof(*free_next),
			 PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (free_next == MAP_FAILED) {
		munmap(nodes, nmax * sizeof(struct inode));
		return 1;
	}

	batch_nfree = calloc(nmax / TREE_BATCH, sizeof(*batch_nfree));
	if (!batch_nfree) {
		munmap(free_next, nmax * sizeof(*free_next));
		munmap(nodes, nmax * sizeof(struct inode));
		return 1;
	}

	for (i = 0; i < TOP_INODES; i++) {
		ino = top[i].ino;
		n = nodes + ino;
		n->ino = ino;
		n->parent = nodes + top[i].parent_ino;
		n->name = top[i].name;
		n->type = top[i].type;
		n->mode = top[i].mode;
		n->flags = INODE_STATIC;
		n->ops = get_inode_ops(n->type);

		if (n->ino == ROOT)
			continue;

		if (n->parent->child) {
			n->sibling = n->parent->child->sibling;
			n->parent->child->sibling = n;
		} else {
			n->parent->child = n;
			n->sibling = n;
		}
	}

	nhwm = TOP_INODES;
	grow();

	return 0;
}
