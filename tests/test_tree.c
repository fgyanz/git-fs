#include "harness.h"
#include "../inode.h"
#include "../tree.h"

#include <git2.h>
#include <pthread.h>

/* inode.c needs it to link; tree tests don't use git */
git_odb *get_gitfs_odb(void) { return NULL; }

extern struct inode *nodes;
extern unsigned long *free_next;

TEST(test_tree_init)
{
	struct inode *root, *n;

	root = get_tree_node(ROOT);
	ASSERT_NOT_NULL(root);
	ASSERT_STR_EQ(root->name, "/");
	ASSERT_EQ(root->mode, T_DIR);
	ASSERT_EQ(root->ino, ROOT);

	n = get_tree_node(BRANCHES);
	ASSERT_NOT_NULL(n);
	ASSERT_STR_EQ(n->name, "branches");
	ASSERT_EQ(n->u.parent->ino, ROOT);

	n = get_tree_node(TAGS);
	ASSERT_NOT_NULL(n);
	ASSERT_STR_EQ(n->name, "tags");
	ASSERT_EQ(n->u.parent->ino, ROOT);

	n = get_tree_node(OBJECTS);
	ASSERT_NOT_NULL(n);
	ASSERT_STR_EQ(n->name, "objects");
	ASSERT_EQ(n->u.parent->ino, ROOT);
	ASSERT_EQ(n->type, T_OBJECTS);
	ASSERT_NOT_NULL(n->ops);

	n = get_tree_node(HEAD);
	ASSERT_NOT_NULL(n);
	ASSERT_STR_EQ(n->name, "HEAD");
	ASSERT_EQ(n->u.parent->ino, ROOT);
	ASSERT_EQ(n->type, T_HEAD);

	n = get_tree_node(HEADS);
	ASSERT_NOT_NULL(n);
	ASSERT_STR_EQ(n->name, "heads");
	ASSERT_EQ(n->u.parent->ino, BRANCHES);
	ASSERT_EQ(n->type, T_BRANCHES);

	n = get_tree_node(REMOTES);
	ASSERT_NOT_NULL(n);
	ASSERT_STR_EQ(n->name, "remotes");
	ASSERT_EQ(n->u.parent->ino, BRANCHES);
	ASSERT_EQ(n->type, T_REMOTES);

	return 0;
}

TEST(test_tree_init_children_findable)
{
	struct inode *root, *n;

	root = get_tree_node(ROOT);

	n = get_tree_child(root, "branches");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->ino, BRANCHES);

	n = get_tree_child(root, "tags");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->ino, TAGS);

	n = get_tree_child(root, "HEAD");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->ino, HEAD);

	return 0;
}

TEST(test_add_node)
{
	struct inode *parent, *child;

	parent = get_tree_node(TAGS);

	child = add_tree_node(parent, "v1.0", T_COMMIT, T_DIR, NULL, NULL);
	ASSERT_NOT_NULL(child);
	ASSERT_STR_EQ(child->name, "v1.0");
	ASSERT_EQ(child->type, T_COMMIT);
	ASSERT_EQ(child->mode, T_DIR);
	ASSERT_EQ(child->u.parent->ino, TAGS);

	ASSERT_EQ((long)get_tree_child(parent, "v1.0"), (long)child);

	return 0;
}

TEST(test_add_duplicate)
{
	struct inode *parent, *first, *second;

	parent = get_tree_node(TAGS);

	first = add_tree_node(parent, "dup-test", T_COMMIT, T_DIR, NULL, NULL);
	ASSERT_NOT_NULL(first);

	second = add_tree_node(parent, "dup-test", T_COMMIT, T_DIR, NULL, NULL);
	ASSERT_EQ((long)first, (long)second);

	return 0;
}

TEST(test_get_tree_node_valid)
{
	struct inode *n;

	n = get_tree_node(ROOT);
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->ino, ROOT);

	n = get_tree_node(BRANCHES);
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->ino, BRANCHES);

	return 0;
}

TEST(test_get_tree_node_invalid)
{
	ASSERT_NULL(get_tree_node(999999999));

	return 0;
}

TEST(test_get_tree_child_not_found)
{
	struct inode *root;

	root = get_tree_node(ROOT);
	ASSERT_NULL(get_tree_child(root, "nonexistent"));

	return 0;
}

TEST(test_get_tree_child_empty_dir)
{
	struct inode *parent;

	parent = get_tree_node(OBJECTS);
	ASSERT_NULL(get_tree_child(parent, "anything"));

	return 0;
}

TEST(test_count_children)
{
	struct inode *parent;
	size_t before, after;

	parent = get_tree_node(OBJECTS);
	before = count_tree_children(parent);
	ASSERT_EQ(before, 0);

	add_tree_node(parent, "child1", T_GENERIC, T_DIR, NULL, NULL);
	add_tree_node(parent, "child2", T_GENERIC, T_DIR, NULL, NULL);
	add_tree_node(parent, "child3", T_GENERIC, T_DIR, NULL, NULL);

	after = count_tree_children(parent);
	ASSERT_EQ(after, 3);

	return 0;
}

TEST(test_get_tree_sibling)
{
	struct inode *parent, *first, *a;

	parent = get_tree_node(REMOTES);

	a = add_tree_node(parent, "origin", T_BRANCHES, T_DIR, NULL, NULL);
	add_tree_node(parent, "upstream", T_BRANCHES, T_DIR, NULL, NULL);
	add_tree_node(parent, "fork", T_BRANCHES, T_DIR, NULL, NULL);

	/* new nodes go after parent->child, so the first one stays head */
	first = parent->child;
	ASSERT_EQ((long)first, (long)a);

	ASSERT_EQ((long)get_tree_sibling(first, 1), (long)first->sibling);

	ASSERT_EQ((long)get_tree_sibling(first, 3), (long)first);

	return 0;
}

TEST(test_circular_list_integrity)
{
	struct inode *parent, *n;
	int count, i;

	parent = get_tree_node(HEADS);

	add_tree_node(parent, "main", T_COMMIT, T_DIR, NULL, NULL);
	add_tree_node(parent, "develop", T_COMMIT, T_DIR, NULL, NULL);
	add_tree_node(parent, "feature-x", T_COMMIT, T_DIR, NULL, NULL);
	add_tree_node(parent, "release-1", T_COMMIT, T_DIR, NULL, NULL);

	n = parent->child;
	count = 0;
	i = 0;
	do {
		count++;
		n = n->sibling;
		if (++i > 100)
			FAIL("circular list is broken (infinite loop)");
	} while (n != parent->child);

	ASSERT_EQ(count, 4);
	ASSERT_EQ(count_tree_children(parent), 4);

	return 0;
}

TEST(test_tree_path)
{
	struct inode *commit_node, *tree_node, *dir_a, *file_b;
	struct inode *tree_out = NULL;
	char path[512] = {0};

	commit_node = get_tree_node(HEAD);

	tree_node = add_tree_node(commit_node, "tree", T_TREE, T_DIR, NULL, NULL);
	dir_a = add_tree_node(tree_node, "src", T_TREE, T_DIR, NULL, NULL);
	file_b = add_tree_node(dir_a, "main.c", T_TREE, T_FILE, NULL, NULL);

	tree_path(file_b, path, sizeof(path), &tree_out);

	/* the top tree node: its parent isn't a T_TREE */
	ASSERT_NOT_NULL(tree_out);
	ASSERT_EQ((long)tree_out, (long)tree_node);

	ASSERT_STR_EQ(path, "/src/main.c");

	return 0;
}

TEST(test_get_tree_sibling_large)
{
	struct inode *parent;
	struct inode *n;
	char name[32];
	int i, count;

	parent = add_tree_node(get_tree_node(ROOT), "large-dir",
	                       T_GENERIC, T_DIR, NULL, NULL);

	for (i = 0; i < 500; i++) {
		snprintf(name, sizeof(name), "entry-%03d", i);
		add_tree_node(parent, name, T_GENERIC, T_FILE, NULL, NULL);
	}

	ASSERT_EQ(count_tree_children(parent), 500);

	/* readdir: offset 0 is p->child, any other offset n is
	 * get_tree_sibling(p->child, n) */
	count = 0;
	for (i = 0; i < 500; i++) {
		if (i == 0)
			n = parent->child;
		else
			n = get_tree_sibling(parent->child, i);

		ASSERT_NOT_NULL(n);
		ASSERT(n->name != NULL);
		count++;
	}
	ASSERT_EQ(count, 500);

	ASSERT((long)get_tree_sibling(parent->child, 1) !=
	       (long)parent->child);
	ASSERT((long)get_tree_sibling(parent->child, 250) !=
	       (long)parent->child);
	ASSERT((long)get_tree_sibling(parent->child, 499) !=
	       (long)parent->child);

	ASSERT_EQ((long)get_tree_sibling(parent->child, 500),
	          (long)parent->child);

	return 0;
}

TEST(test_readdir_offset_no_duplicates)
{
	struct inode *parent, *seen[200];
	struct inode *n;
	char name[32];
	int i, j, dup;

	parent = add_tree_node(get_tree_node(ROOT), "nodup-dir",
	                       T_GENERIC, T_DIR, NULL, NULL);

	for (i = 0; i < 200; i++) {
		snprintf(name, sizeof(name), "file-%03d", i);
		add_tree_node(parent, name, T_GENERIC, T_FILE, NULL, NULL);
	}

	for (i = 0; i < 200; i++) {
		if (i == 0)
			n = parent->child;
		else
			n = get_tree_sibling(parent->child, i);

		dup = 0;
		for (j = 0; j < i; j++) {
			if (seen[j] == n) {
				dup = 1;
				break;
			}
		}
		if (dup)
			FAIL("duplicate node at offset %d: %s", i, n->name);

		seen[i] = n;
	}

	return 0;
}

TEST(test_deleted_skip)
{
	struct inode *parent, *n;

	parent = add_tree_node(get_tree_node(ROOT), "tomb-parent",
	                       T_GENERIC, T_DIR, NULL, NULL);
	n = add_tree_node(parent, "alive", T_GENERIC, T_FILE, NULL, NULL);
	add_tree_node(parent, "dead", T_GENERIC, T_FILE, NULL, NULL);

	afor(&get_tree_child(parent, "dead")->flags,
	                  INODE_DELETED);

	ASSERT_NULL(get_tree_child(parent, "dead"));
	ASSERT_EQ((long)get_tree_child(parent, "alive"), (long)n);

	ASSERT_EQ(count_tree_children(parent), 1);

	return 0;
}

TEST(test_ref_forget_keeps_cache)
{
	struct inode *parent, *n, *cached;

	parent = add_tree_node(get_tree_node(ROOT), "ref-parent",
	                       T_GENERIC, T_DIR, NULL, NULL);
	n = add_tree_node(parent, "ref-child", T_GENERIC, T_FILE, NULL, NULL);

	tree_ref(n);
	tree_ref(n);
	ASSERT_EQ(n->nlookup, 2);

	tree_forget(n, 1);
	ASSERT_EQ(n->nlookup, 1);
	ASSERT_NOT_NULL(get_tree_child(parent, "ref-child"));

	tree_forget(n, 1);
	ASSERT_EQ(n->nlookup, 0);

	/* nlookup 0 keeps a live-ring node cached: freeing its name would
	 * race a get_tree_child() in the middle of strcmp() */
	cached = get_tree_child(parent, "ref-child");
	ASSERT_EQ((long)cached, (long)n);
	ASSERT_STR_EQ(cached->name, "ref-child");

	return 0;
}

TEST(test_forget_preserves_name_on_live_ring)
{
	struct inode *parent, *n;

	parent = add_tree_node(get_tree_node(ROOT), "name-stable",
	                       T_GENERIC, T_DIR, NULL, NULL);
	n = add_tree_node(parent, "stable-name", T_GENERIC, T_FILE, NULL, NULL);

	tree_ref(n);
	tree_forget(n, 1);

	ASSERT_NOT_NULL(n->name);
	ASSERT_STR_EQ(n->name, "stable-name");

	return 0;
}

TEST(test_forget_static)
{
	struct inode *root;

	root = get_tree_node(ROOT);
	tree_ref(root);
	tree_forget(root, 1);

	ASSERT_NOT_NULL(get_tree_node(ROOT));
	ASSERT_STR_EQ(root->name, "/");

	return 0;
}

TEST(test_forget_detached)
{
	struct inode *parent, *n;
	unsigned long ino;

	parent = add_tree_node(get_tree_node(ROOT), "orphan-parent",
	                       T_GENERIC, T_DIR, NULL, NULL);
	n = add_tree_node(parent, "orphan-child", T_GENERIC, T_FILE, NULL, NULL);
	ino = n->ino;

	tree_ref(n);
	afor(&n->flags, INODE_DETACHED);

	tree_forget(n, 1);

	ASSERT(aload(&free_next[ino]) != 0);

	return 0;
}

TEST(test_free_retired)
{
	struct inode *parent, *old;
	unsigned long ino1, ino2;

	parent = add_tree_node(get_tree_node(ROOT), "retire-parent",
	                       T_GENERIC, T_DIR, NULL, NULL);
	add_tree_node(parent, "r1", T_GENERIC, T_FILE, NULL, NULL);
	add_tree_node(parent, "r2", T_GENERIC, T_FILE, NULL, NULL);

	ino1 = get_tree_child(parent, "r1")->ino;
	ino2 = get_tree_child(parent, "r2")->ino;

	old = axchg(&parent->child, NULL);
	ASSERT_NOT_NULL(old);
	ASSERT_EQ(count_tree_children(parent), 0);

	free_retired(old);

	ASSERT(aload(&free_next[ino1]) != 0);
	ASSERT(aload(&free_next[ino2]) != 0);

	return 0;
}

TEST(test_rebuild_preserves_live)
{
	struct inode *parent, *live, *old;
	unsigned long live_ino;

	parent = add_tree_node(get_tree_node(ROOT), "rebuild-parent",
	                       T_GENERIC, T_DIR, NULL, NULL);
	live = add_tree_node(parent, "live-node", T_GENERIC, T_FILE, NULL, NULL);
	add_tree_node(parent, "dead-node", T_GENERIC, T_FILE, NULL, NULL);

	tree_ref(live);
	live_ino = live->ino;

	old = axchg(&parent->child, NULL);
	free_retired(old);

	live = nodes + live_ino;
	ASSERT_EQ(aload(&live->flags) & INODE_DETACHED,
	          INODE_DETACHED);
	ASSERT_EQ(aload(&live->nlookup), 1);

	tree_forget(live, 1);
	ASSERT(aload(&free_next[live_ino]) != 0);

	return 0;
}

TEST(test_pool_grow)
{
	struct inode *parent, *n;
	char name[32];
	int i;

	parent = add_tree_node(get_tree_node(ROOT), "grow-parent",
	                       T_GENERIC, T_DIR, NULL, NULL);

	for (i = 0; i < 5000; i++) {
		snprintf(name, sizeof(name), "grow-%04d", i);
		n = add_tree_node(parent, name, T_GENERIC, T_FILE, NULL, NULL);
		ASSERT_NOT_NULL(n);
	}

	ASSERT_EQ(count_tree_children(parent), 5000);

	return 0;
}

TEST(test_deleted_in_sibling_walk)
{
	struct inode *parent, *b;

	parent = add_tree_node(get_tree_node(ROOT), "sib-walk",
	                       T_GENERIC, T_DIR, NULL, NULL);
	add_tree_node(parent, "sa", T_GENERIC, T_FILE, NULL, NULL);
	b = add_tree_node(parent, "sb", T_GENERIC, T_FILE, NULL, NULL);
	add_tree_node(parent, "sc", T_GENERIC, T_FILE, NULL, NULL);

	afor(&b->flags, INODE_DELETED);

	ASSERT_EQ(count_tree_children(parent), 2);

	ASSERT_NOT_NULL(get_tree_child(parent, "sa"));
	ASSERT_NOT_NULL(get_tree_child(parent, "sc"));

	return 0;
}

TEST(test_rebuild_restore_on_failure)
{
	struct inode *parent, *c;

	parent = add_tree_node(get_tree_node(ROOT), "restore-parent",
	                       T_GENERIC, T_DIR, NULL, NULL);
	add_tree_node(parent, "keep-me", T_GENERIC, T_FILE, NULL, NULL);

	ASSERT_EQ(count_tree_children(parent), 1);

	astore(&parent->retired, axchg(&parent->child, NULL));
	ASSERT_EQ(count_tree_children(parent), 0);

	astore(&parent->child, axchg(&parent->retired, NULL));
	ASSERT_EQ(count_tree_children(parent), 1);

	c = get_tree_child(parent, "keep-me");
	ASSERT_NOT_NULL(c);

	return 0;
}

TEST(test_free_retired_skips_static)
{
	struct inode *parent, *s, *old;
	unsigned long s_ino;

	parent = add_tree_node(get_tree_node(ROOT), "skip-static",
	                       T_GENERIC, T_DIR, NULL, NULL);
	s = add_tree_node(parent, "fake-static", T_GENERIC, T_FILE, NULL, NULL);
	s->flags |= INODE_STATIC;
	s_ino = s->ino;

	add_tree_node(parent, "dynamic", T_GENERIC, T_FILE, NULL, NULL);

	old = axchg(&parent->child, NULL);
	free_retired(old);

	s = nodes + s_ino;
	ASSERT_EQ(aload(&free_next[s_ino]), 0);
	ASSERT_STR_EQ(s->name, "fake-static");

	return 0;
}

/* walks p's children like gitfs_readdir(); returns the live ones */
static int
cursor_walk(struct inode *p, struct inode **seen, int max)
{
	struct inode *head, *cur;
	int count = 0;

	head = aload(&p->child);
	cur = head;

	while (cur) {
		while (aload(&cur->flags) & INODE_DELETED) {
			cur = aload(&cur->sibling);
			if (cur == head) {
				cur = NULL;
				break;
			}
		}
		if (!cur)
			break;

		if (count < max)
			seen[count] = cur;
		count++;

		cur = aload(&cur->sibling);
		if (cur == head)
			cur = NULL;
	}

	return count;
}

TEST(test_cursor_walk_all_nodes)
{
	struct inode *parent, *seen[200];
	char name[32];
	int i, j, count;

	parent = add_tree_node(get_tree_node(ROOT), "cursor-all",
	                       T_GENERIC, T_DIR, NULL, NULL);

	for (i = 0; i < 200; i++) {
		snprintf(name, sizeof(name), "c-%03d", i);
		add_tree_node(parent, name, T_GENERIC, T_FILE, NULL, NULL);
	}

	count = cursor_walk(parent, seen, 200);
	ASSERT_EQ(count, 200);

	for (i = 0; i < count; i++)
		for (j = i + 1; j < count; j++)
			if (seen[i] == seen[j])
				FAIL("duplicate at %d and %d: %s",
				     i, j, seen[i]->name);

	return 0;
}

TEST(test_cursor_walk_skip_dead)
{
	struct inode *parent, *b, *seen[4];
	int count;

	parent = add_tree_node(get_tree_node(ROOT), "cursor-dead",
	                       T_GENERIC, T_DIR, NULL, NULL);
	add_tree_node(parent, "ca", T_GENERIC, T_FILE, NULL, NULL);
	b = add_tree_node(parent, "cb", T_GENERIC, T_FILE, NULL, NULL);
	add_tree_node(parent, "cc", T_GENERIC, T_FILE, NULL, NULL);

	afor(&b->flags, INODE_DELETED);

	count = cursor_walk(parent, seen, 4);
	ASSERT_EQ(count, 2);

	for (int i = 0; i < count; i++)
		ASSERT(seen[i] != b);

	return 0;
}

TEST(test_cursor_walk_empty)
{
	struct inode *parent, *seen[1];
	int count;

	parent = add_tree_node(get_tree_node(ROOT), "cursor-empty",
	                       T_GENERIC, T_DIR, NULL, NULL);

	count = cursor_walk(parent, seen, 1);
	ASSERT_EQ(count, 0);

	return 0;
}

TEST(test_cursor_walk_single)
{
	struct inode *parent, *only, *seen[1];
	int count;

	parent = add_tree_node(get_tree_node(ROOT), "cursor-single",
	                       T_GENERIC, T_DIR, NULL, NULL);
	only = add_tree_node(parent, "only-child", T_GENERIC, T_FILE, NULL, NULL);

	count = cursor_walk(parent, seen, 1);
	ASSERT_EQ(count, 1);
	ASSERT_EQ((long)seen[0], (long)only);

	return 0;
}

TEST(test_cursor_walk_all_dead)
{
	struct inode *parent, *seen[1];
	int count;

	parent = add_tree_node(get_tree_node(ROOT), "cursor-alldead",
	                       T_GENERIC, T_DIR, NULL, NULL);
	afor(&add_tree_node(parent, "d1", T_GENERIC, T_FILE, NULL, NULL)->flags,
	     INODE_DELETED);
	afor(&add_tree_node(parent, "d2", T_GENERIC, T_FILE, NULL, NULL)->flags,
	     INODE_DELETED);
	afor(&add_tree_node(parent, "d3", T_GENERIC, T_FILE, NULL, NULL)->flags,
	     INODE_DELETED);

	count = cursor_walk(parent, seen, 1);
	ASSERT_EQ(count, 0);

	return 0;
}

TEST(test_get_tree_node_boundary)
{
	ASSERT_NULL(get_tree_node(200000));
	ASSERT_NULL(get_tree_node((unsigned long)-1));

	return 0;
}

TEST(test_add_node_has_ops)
{
	struct inode *parent, *child;

	parent = get_tree_node(ROOT);
	child = add_tree_node(parent, "test-ops", T_GENERIC, T_DIR, NULL, NULL);
	ASSERT_NOT_NULL(child);
	ASSERT_NOT_NULL(child->ops);

	ASSERT_NULL(get_inode_ops(T_ALL));
	ASSERT_NULL(get_inode_ops(999));

	return 0;
}

TEST(test_tree_path_shallow)
{
	struct inode *parent, *tree_node;
	struct inode *tree_out = NULL;
	char path[512] = {0};

	parent = get_tree_node(HEAD);
	tree_node = add_tree_node(parent, "shallow-tree", T_TREE, T_DIR, NULL, NULL);

	tree_path(tree_node, path, sizeof(path), &tree_out);
	ASSERT_EQ((long)tree_out, (long)tree_node);
	ASSERT_STR_EQ(path, "");

	return 0;
}

TEST(test_count_children_after_dedup)
{
	struct inode *parent;

	parent = add_tree_node(get_tree_node(ROOT), "dedup-parent",
	                       T_GENERIC, T_DIR, NULL, NULL);

	add_tree_node(parent, "x", T_GENERIC, T_DIR, NULL, NULL);
	add_tree_node(parent, "y", T_GENERIC, T_DIR, NULL, NULL);

	add_tree_node(parent, "x", T_GENERIC, T_DIR, NULL, NULL);
	add_tree_node(parent, "y", T_GENERIC, T_DIR, NULL, NULL);
	ASSERT_EQ(count_tree_children(parent), 2);

	return 0;
}

static size_t
live_inodes(void)
{
	struct tree_stats st;

	tree_stats(&st);
	return st.live;
}

TEST(test_stats_live_accounting)
{
	struct inode *parent, *held, *old;
	size_t base;

	base = live_inodes();

	parent = add_tree_node(get_tree_node(ROOT), "stats-parent",
	                       T_GENERIC, T_DIR, NULL, NULL);
	add_tree_node(parent, "s1", T_GENERIC, T_FILE, NULL, NULL);
	held = add_tree_node(parent, "s2", T_GENERIC, T_FILE, NULL, NULL);
	/* existing name: no new inode */
	add_tree_node(parent, "s1", T_GENERIC, T_FILE, NULL, NULL);
	ASSERT_EQ(live_inodes(), base + 3);

	/* s1 is freed right away, s2 waits for the kernel's forget */
	tree_ref(held);
	old = axchg(&parent->child, NULL);
	free_retired(old);
	ASSERT_EQ(live_inodes(), base + 2);

	tree_forget(held, 1);
	ASSERT_EQ(live_inodes(), base + 1);

	return 0;
}

TEST(test_retire_frees_descendants)
{
	struct inode *parent, *idle, *held, *sub, *old;
	char name[32];
	size_t base;
	int i;

	parent = add_tree_node(get_tree_node(ROOT), "desc-parent",
	                       T_GENERIC, T_DIR, NULL, NULL);
	base = live_inodes();

	idle = add_tree_node(parent, "idle", T_GENERIC, T_DIR, NULL, NULL);
	held = add_tree_node(parent, "held", T_GENERIC, T_DIR, NULL, NULL);
	sub = add_tree_node(idle, "sub", T_GENERIC, T_DIR, NULL, NULL);
	for (i = 0; i < 16; i++) {
		snprintf(name, sizeof(name), "f-%02d", i);
		add_tree_node(sub, name, T_GENERIC, T_FILE, NULL, NULL);
		add_tree_node(held, name, T_GENERIC, T_FILE, NULL, NULL);
	}

	tree_ref(held);
	old = axchg(&parent->child, NULL);
	free_retired(old);
	tree_forget(held, 1);

	if (live_inodes() != base)
		FAIL("%zu descendant inodes leaked", live_inodes() - base);

	return 0;
}

#define DEDUP_THREADS 8
#define DEDUP_NAMES   256

static pthread_barrier_t dedup_barrier;

static void *
dedup_worker(void *arg)
{
	struct inode *p = arg;
	char name[32];
	int i;

	pthread_barrier_wait(&dedup_barrier);
	for (i = 0; i < DEDUP_NAMES; i++) {
		snprintf(name, sizeof(name), "d-%03d", i);
		add_tree_node(p, name, T_GENERIC, T_FILE, NULL, NULL);
	}
	return NULL;
}

static int
race_add_same_names(struct inode *p, size_t *ring, size_t *dead)
{
	pthread_t tid[DEDUP_THREADS];
	struct inode *head, *c;
	int i;

	pthread_barrier_init(&dedup_barrier, NULL, DEDUP_THREADS);
	for (i = 0; i < DEDUP_THREADS; i++)
		if (pthread_create(&tid[i], NULL, dedup_worker, p))
			return -1;
	for (i = 0; i < DEDUP_THREADS; i++)
		pthread_join(tid[i], NULL);
	pthread_barrier_destroy(&dedup_barrier);

	*ring = *dead = 0;
	head = aload(&p->child);
	c = head;
	do {
		(*ring)++;
		if (aload(&c->flags) & INODE_DELETED)
			(*dead)++;
		c = aload(&c->sibling);
	} while (c != head);

	return 0;
}

/* dead duplicates are fine, unreachable inodes are not */
TEST(test_concurrent_add_accounting)
{
	struct inode *p;
	size_t base, ring, dead;

	p = add_tree_node(get_tree_node(ROOT), "dedup-acct",
	                  T_GENERIC, T_DIR, NULL, NULL);
	base = live_inodes();

	if (race_add_same_names(p, &ring, &dead))
		FAIL("pthread_create failed");
	ASSERT_EQ(live_inodes() - base, ring);

	return 0;
}

TEST(test_concurrent_add_no_dups)
{
	struct inode *p;
	size_t ring, dead;

	p = add_tree_node(get_tree_node(ROOT), "dedup-dups",
	                  T_GENERIC, T_DIR, NULL, NULL);

	if (race_add_same_names(p, &ring, &dead))
		FAIL("pthread_create failed");
	if (ring - dead != DEDUP_NAMES)
		FAIL("%zu live inodes for %d names",
		     ring - dead, DEDUP_NAMES);

	return 0;
}

TEST(test_tree_destroy)
{
	tree_destroy();
	ASSERT_NULL(nodes);

	return 0;
}

int
main(void)
{
	fprintf(stderr, "test_tree:\n");

	if (tree_init()) {
		fprintf(stderr, "tree_init failed\n");
		return EXIT_FAILURE;
	}

	RUN_TEST(test_tree_init);
	RUN_TEST(test_tree_init_children_findable);
	RUN_TEST(test_add_node);
	RUN_TEST(test_add_duplicate);
	RUN_TEST(test_get_tree_node_valid);
	RUN_TEST(test_get_tree_node_invalid);
	RUN_TEST(test_get_tree_child_not_found);
	RUN_TEST(test_get_tree_child_empty_dir);
	RUN_TEST(test_count_children);
	RUN_TEST(test_get_tree_sibling);
	RUN_TEST(test_circular_list_integrity);
	RUN_TEST(test_tree_path);
	RUN_TEST(test_get_tree_sibling_large);
	RUN_TEST(test_readdir_offset_no_duplicates);
	RUN_TEST(test_get_tree_node_boundary);
	RUN_TEST(test_add_node_has_ops);
	RUN_TEST(test_tree_path_shallow);
	RUN_TEST(test_count_children_after_dedup);

	RUN_TEST(test_cursor_walk_all_nodes);
	RUN_TEST(test_cursor_walk_skip_dead);
	RUN_TEST(test_cursor_walk_empty);
	RUN_TEST(test_cursor_walk_single);
	RUN_TEST(test_cursor_walk_all_dead);

	RUN_TEST(test_deleted_skip);
	RUN_TEST(test_ref_forget_keeps_cache);
	RUN_TEST(test_forget_preserves_name_on_live_ring);
	RUN_TEST(test_forget_static);
	RUN_TEST(test_forget_detached);
	RUN_TEST(test_free_retired);
	RUN_TEST(test_rebuild_preserves_live);
	RUN_TEST(test_pool_grow);
	RUN_TEST(test_deleted_in_sibling_walk);
	RUN_TEST(test_rebuild_restore_on_failure);
	RUN_TEST(test_free_retired_skips_static);

	RUN_TEST(test_stats_live_accounting);
	RUN_TEST(test_concurrent_add_accounting);
	RUN_XFAIL(test_concurrent_add_no_dups,
	          "add_tree_node() dedup keeps racing inserts alive");
	RUN_XFAIL(test_retire_frees_descendants,
	          "push_free() drops the subtree of a retired node");

	/* must be last: tears down the pool */
	RUN_TEST(test_tree_destroy);

	TEST_SUMMARY();
}
