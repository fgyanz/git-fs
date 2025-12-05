#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inode.h"
#include "tree.h"

#define TREE_SIZE        100000
#define MAX_TREE_DEPTH   200

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

static struct inode *nodes;
static struct inode *free_top;
static size_t nnodes;

static struct inode *
new_tree_node(void)
{
	struct inode *n;

	assert(free_top != NULL);

	n = free_top;
	free_top = n->next_free;
	n->next_free = NULL;
	return n;
}

struct inode *
add_tree_node(struct inode *parent, const char *name, unsigned type, mode_t mode)
{
	struct inode *n;

	n = get_tree_child(parent, name);
	if (n)
		return n;

	n = new_tree_node();
	n->name = strdup(name);
	n->ops = get_inode_ops(type);
	n->mode = mode;
	n->parent = parent;
	n->type = type;

	if (!parent->child) {
		parent->child = n;
		n->sibling = n;
	} else {
		n->sibling = parent->child->sibling;
		parent->child->sibling = n;
	}

	return n;
}

struct inode *
get_tree_node(unsigned long ino)
{
	struct inode *n;

	if (ino > nnodes)
		return NULL;

	n = ino + nodes;
	if (n->next_free)
		return NULL;

	return n;
}

struct inode *
get_tree_child(struct inode *parent, const char *name)
{
	struct inode *c, *s;
	unsigned long ino;
	int i;

	c = parent->child;
	if (!c)
		return NULL;

	do {
		if (!strcmp(c->name, name))
			return c;
		c = c->sibling;
	} while (c != parent->child);

	return NULL;
}

struct inode *
get_tree_sibling(struct inode *node, off_t pos)
{
	struct inode *s;

	s = node;
	do {
		s = s->sibling;
	} while (s != node && --pos > 0);

	return s;
}

size_t
count_tree_children(struct inode *parent)
{
	struct inode *n, *s;
	size_t children;

	n = parent->child;
	if (!n)
		return 0;

	s = n->sibling;
	for (children = 1; s != n; children++, s = s->sibling);

	return children;
}

void
tree_path(struct inode *node, char *opath, size_t size, struct inode **tree)
{
	char *v[MAX_TREE_DEPTH], **vstart, **vend;
	int len;

	vstart = v;
	vend = v + sizeof(v);
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

int
tree_init(void)
{
	int i;
	unsigned long ino;
	struct inode *n;

	nnodes = TOP_INODES + TREE_SIZE;

	nodes = calloc(nnodes, sizeof(struct inode));
	if (!nodes) {
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
		n->ops = get_inode_ops(n->type);

		if (n->ino == ROOT)
			continue;

		// circular linked list of siblings
		if (n->parent->child) {
			n->sibling = n->parent->child->sibling;
			n->parent->child->sibling = n;
		} else {
			n->parent->child = n;
			n->sibling = n;
		}
	}

	for (; i < nnodes; i++) {
		n = nodes + i;
		n->ino = i;
		n->next_free = free_top;
		free_top = nodes + i;
	}

	return 0;
}
