#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <git2.h>

#include "inode.h"
#include "tree.h"

#define TREE_MAX         (16 << 20)
#define TREE_BATCH       4096
#define MAX_TREE_DEPTH   200

/* marks the end of the free list (so next_free is never NULL on it) */
static struct inode free_end;

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
static struct inode *ftop;   /* free list head */
static size_t nhwm;          /* high-water mark: inodes past this are unused */
static size_t nmax;          /* total mmap capacity */

/* clear the node and put it back on the free list.
 * uses relaxed orderings since the swap loop on ftop is self-contained. */
static void
push_free(struct inode *n)
{
	struct inode *t;
	unsigned long ino = n->ino;

	memset(n, 0, sizeof(*n));
	n->ino = ino;

	do {
		t = __atomic_load_n(&ftop, __ATOMIC_RELAXED);
		__atomic_store_n(&n->next_free, t ? t : &free_end,
				 __ATOMIC_RELAXED);
	} while (!__atomic_compare_exchange_n(&ftop, &t, n, 1,
			__ATOMIC_RELEASE, __ATOMIC_RELAXED));
}

/* extend the free list with a new batch from the mmap pool.
 * only one thread wins the swap on nhwm; the rest return. */
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
		push_free(nodes + i);
	}
}

/* pop a node from the free list. grows the pool if empty. */
static struct inode *
new_node(void)
{
	struct inode *n, *nx;

	for (;;) {
		n = aload(&ftop);
		if (!n) {
			grow();
			n = aload(&ftop);
			if (!n)
				return NULL;
		}
		nx = __atomic_load_n(&n->next_free, __ATOMIC_RELAXED);
		if (nx == &free_end)
			nx = NULL;
		if (acas(&ftop, &n, nx))
			break;
	}

	astore(&n->next_free, NULL);
	return n;
}

static inline int
is_dead(struct inode *n)
{
	return aload(&n->flags) & INODE_DELETED;
}

/* add a child to p's circular sibling list.
 * if a child with the same name exists, return it instead.
 * empty list: self-loop. non-empty: insert after head. */
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

/* look up an inode by number. returns NULL if freed, deleted, or out of range. */
struct inode *
get_tree_node(unsigned long ino)
{
	struct inode *n;

	if (ino >= aload(&nhwm))
		return NULL;

	n = nodes + ino;
	if (aload(&n->next_free))
		return NULL;
	if (is_dead(n))
		return NULL;

	return n;
}

/* find a live child of p by name. walks the circular sibling list. */
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

/* walk pos live siblings forward from node. used by readdir for offsets. */
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

/* count live children of p (skips deleted nodes). */
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

/* build the relative path from a nested tree node up to its root tree.
 * walks parent pointers until it leaves T_TREE nodes, then reconstructs
 * the path in forward order. */
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

/* free the name and git object held by a node. */
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

/* increment the kernel reference count on n. called on each lookup reply. */
void
tree_ref(struct inode *n)
{
	afadd(&n->nlookup, 1);
}

/* drop nlookup references. when it reaches zero, mark the node as deleted
 * and free its resources. if the node was already detached from its parent
 * (by free_retired), reclaim the slot immediately. */
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

	/* flags may have changed since our first read */
	f = aload(&n->flags);
	if (f & INODE_DETACHED)
		push_free(n);
}

/* walk a retired circular sibling list and reclaim nodes.
 * - unreferenced nodes: free resources and return slot to the pool.
 * - referenced nodes (kernel still uses them): mark as detached so
 *   tree_forget reclaims them later.
 * - static nodes: skip. */
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

/* free all remaining nodes and release the mmap pool. called at shutdown. */
void
tree_destroy(void)
{
	size_t i, hwm;

	if (!nodes)
		return;

	hwm = aload(&nhwm);
	for (i = TOP_INODES; i < hwm; i++) {
		struct inode *n = nodes + i;
		if (aload(&n->next_free))
			continue;
		clear_node(n);
	}

	munmap(nodes, nmax * sizeof(struct inode));
	nodes = NULL;
}

/* reserve the mmap pool, set up the static inodes, and fill the free list. */
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
