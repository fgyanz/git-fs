#include "harness.h"
#include "../inode.h"
#include "../tree.h"

extern struct inode *nodes;

TEST(test_tree_init)
{
	struct inode *root, *n;

	root = get_tree_node(ROOT);
	ASSERT_NOT_NULL(root);
	ASSERT_STR_EQ(root->name, "/");
	ASSERT_EQ(root->mode, T_DIR);
	ASSERT_EQ(root->ino, ROOT);

	/* ROOT's children: branches, tags, objects, HEAD */
	n = get_tree_node(BRANCHES);
	ASSERT_NOT_NULL(n);
	ASSERT_STR_EQ(n->name, "branches");
	ASSERT_EQ(n->parent->ino, ROOT);

	n = get_tree_node(TAGS);
	ASSERT_NOT_NULL(n);
	ASSERT_STR_EQ(n->name, "tags");
	ASSERT_EQ(n->parent->ino, ROOT);

	n = get_tree_node(HEAD);
	ASSERT_NOT_NULL(n);
	ASSERT_STR_EQ(n->name, "HEAD");
	ASSERT_EQ(n->parent->ino, ROOT);
	ASSERT_EQ(n->type, T_COMMIT);

	/* BRANCHES children: heads, remotes */
	n = get_tree_node(HEADS);
	ASSERT_NOT_NULL(n);
	ASSERT_STR_EQ(n->name, "heads");
	ASSERT_EQ(n->parent->ino, BRANCHES);
	ASSERT_EQ(n->type, T_BRANCHES);

	n = get_tree_node(REMOTES);
	ASSERT_NOT_NULL(n);
	ASSERT_STR_EQ(n->name, "remotes");
	ASSERT_EQ(n->parent->ino, BRANCHES);
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

	child = add_tree_node(parent, "v1.0", T_COMMIT, T_DIR);
	ASSERT_NOT_NULL(child);
	ASSERT_STR_EQ(child->name, "v1.0");
	ASSERT_EQ(child->type, T_COMMIT);
	ASSERT_EQ(child->mode, T_DIR);
	ASSERT_EQ(child->parent->ino, TAGS);

	/* Should be findable via get_tree_child */
	ASSERT_EQ((long)get_tree_child(parent, "v1.0"), (long)child);

	return 0;
}

TEST(test_add_duplicate)
{
	struct inode *parent, *first, *second;

	parent = get_tree_node(TAGS);

	first = add_tree_node(parent, "dup-test", T_COMMIT, T_DIR);
	ASSERT_NOT_NULL(first);

	second = add_tree_node(parent, "dup-test", T_COMMIT, T_DIR);
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
	/* Out of range */
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

	/* OBJECTS has no children */
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

	add_tree_node(parent, "child1", T_GENERIC, T_DIR);
	add_tree_node(parent, "child2", T_GENERIC, T_DIR);
	add_tree_node(parent, "child3", T_GENERIC, T_DIR);

	after = count_tree_children(parent);
	ASSERT_EQ(after, 3);

	return 0;
}

TEST(test_get_tree_sibling)
{
	struct inode *parent, *first, *a;

	parent = get_tree_node(REMOTES);

	a = add_tree_node(parent, "origin", T_BRANCHES, T_DIR);
	add_tree_node(parent, "upstream", T_BRANCHES, T_DIR);
	add_tree_node(parent, "fork", T_BRANCHES, T_DIR);

	/*
	 * Circular list insertion: each new node is inserted after
	 * parent->child. parent->child stays as 'a' (first inserted).
	 *
	 * get_tree_sibling(node, pos) is a do-while that always advances
	 * at least once. pos=0 advances once, pos=1 advances once, etc.
	 * This matches readdir usage: offset 0 is handled by using
	 * p->child directly, offset >= 1 uses get_tree_sibling.
	 */
	first = parent->child;
	ASSERT_EQ((long)first, (long)a);

	/* offset 1 advances once from first */
	ASSERT_EQ((long)get_tree_sibling(first, 1), (long)first->sibling);

	/* full cycle: offset == count wraps back to start */
	ASSERT_EQ((long)get_tree_sibling(first, 3), (long)first);

	return 0;
}

TEST(test_circular_list_integrity)
{
	struct inode *parent, *n;
	int count, i;

	parent = get_tree_node(HEADS);

	add_tree_node(parent, "main", T_COMMIT, T_DIR);
	add_tree_node(parent, "develop", T_COMMIT, T_DIR);
	add_tree_node(parent, "feature-x", T_COMMIT, T_DIR);
	add_tree_node(parent, "release-1", T_COMMIT, T_DIR);

	/* Walk the circular list, should visit exactly 4 nodes */
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

	/* Simulate a commit with nested tree:  tree/ -> dir_a/ -> file_b */
	commit_node = get_tree_node(HEAD);

	tree_node = add_tree_node(commit_node, "tree", T_TREE, T_DIR);
	dir_a = add_tree_node(tree_node, "src", T_TREE, T_DIR);
	file_b = add_tree_node(dir_a, "main.c", T_TREE, T_FILE);

	tree_path(file_b, path, sizeof(path), &tree_out);

	/* tree_out should point to the T_TREE node whose parent is NOT T_TREE */
	ASSERT_NOT_NULL(tree_out);
	ASSERT_EQ((long)tree_out, (long)tree_node);

	/* path should be "/src/main.c" */
	ASSERT_STR_EQ(path, "/src/main.c");

	return 0;
}

/* --- Large offset tests --- */

TEST(test_get_tree_sibling_large)
{
	struct inode *parent;
	struct inode *n;
	char name[32];
	int i, count;

	parent = add_tree_node(get_tree_node(ROOT), "large-dir",
	                       T_GENERIC, T_DIR);

	for (i = 0; i < 500; i++) {
		snprintf(name, sizeof(name), "entry-%03d", i);
		add_tree_node(parent, name, T_GENERIC, T_FILE);
	}

	ASSERT_EQ(count_tree_children(parent), 500);

	/*
	 * Simulate readdir offset traversal: offset 0 uses p->child
	 * directly, offset >= 1 uses get_tree_sibling(p->child, offset).
	 *
	 * Walk all 500 entries the same way readdir does and verify
	 * we visit exactly 500 unique nodes with no duplicates.
	 */
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

	/* Verify specific offsets land on different nodes */
	ASSERT((long)get_tree_sibling(parent->child, 1) !=
	       (long)parent->child);
	ASSERT((long)get_tree_sibling(parent->child, 250) !=
	       (long)parent->child);
	ASSERT((long)get_tree_sibling(parent->child, 499) !=
	       (long)parent->child);

	/* Full wrap: offset == count returns to start */
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
	                       T_GENERIC, T_DIR);

	for (i = 0; i < 200; i++) {
		snprintf(name, sizeof(name), "file-%03d", i);
		add_tree_node(parent, name, T_GENERIC, T_FILE);
	}

	/*
	 * Walk like readdir and check for duplicate inodes.
	 * This catches off-by-one errors in the circular list traversal.
	 */
	for (i = 0; i < 200; i++) {
		if (i == 0)
			n = parent->child;
		else
			n = get_tree_sibling(parent->child, i);

		/* Check not seen before */
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

/* --- Memory management tests --- */

TEST(test_deleted_skip)
{
	struct inode *parent, *n;

	parent = add_tree_node(get_tree_node(ROOT), "tomb-parent",
	                       T_GENERIC, T_DIR);
	n = add_tree_node(parent, "alive", T_GENERIC, T_FILE);
	add_tree_node(parent, "dead", T_GENERIC, T_FILE);

	/* mark "dead" as deleted */
	afor(&get_tree_child(parent, "dead")->flags,
	                  INODE_DELETED);

	/* deleted nodes should be skipped */
	ASSERT_NULL(get_tree_child(parent, "dead"));
	ASSERT_EQ((long)get_tree_child(parent, "alive"), (long)n);

	/* count should exclude deleted */
	ASSERT_EQ(count_tree_children(parent), 1);

	return 0;
}

TEST(test_ref_forget)
{
	struct inode *parent, *n;

	parent = add_tree_node(get_tree_node(ROOT), "ref-parent",
	                       T_GENERIC, T_DIR);
	n = add_tree_node(parent, "ref-child", T_GENERIC, T_FILE);

	tree_ref(n);
	tree_ref(n);
	ASSERT_EQ(n->nlookup, 2);

	tree_forget(n, 1);
	ASSERT_EQ(n->nlookup, 1);
	/* Still alive */
	ASSERT_NOT_NULL(get_tree_child(parent, "ref-child"));

	tree_forget(n, 1);
	/* Now deleted */
	ASSERT_NULL(get_tree_child(parent, "ref-child"));

	return 0;
}

TEST(test_forget_static)
{
	struct inode *root;

	root = get_tree_node(ROOT);
	tree_ref(root);
	tree_forget(root, 1);

	/* Static node should survive */
	ASSERT_NOT_NULL(get_tree_node(ROOT));
	ASSERT_STR_EQ(root->name, "/");

	return 0;
}

TEST(test_forget_detached)
{
	struct inode *parent, *n;
	unsigned long ino;

	parent = add_tree_node(get_tree_node(ROOT), "orphan-parent",
	                       T_GENERIC, T_DIR);
	n = add_tree_node(parent, "orphan-child", T_GENERIC, T_FILE);
	ino = n->ino;

	/* detach from parent list */
	tree_ref(n);
	afor(&n->flags, INODE_DETACHED);

	/* Forget should reclaim slot immediately */
	tree_forget(n, 1);

	/* Node should be on free list now */
	n = nodes + ino;
	ASSERT_NOT_NULL(aload(&n->next_free));

	return 0;
}

TEST(test_free_retired)
{
	struct inode *parent, *old;
	unsigned long ino1, ino2;

	parent = add_tree_node(get_tree_node(ROOT), "retire-parent",
	                       T_GENERIC, T_DIR);
	add_tree_node(parent, "r1", T_GENERIC, T_FILE);
	add_tree_node(parent, "r2", T_GENERIC, T_FILE);

	ino1 = get_tree_child(parent, "r1")->ino;
	ino2 = get_tree_child(parent, "r2")->ino;

	/* swap out child list like opendir does */
	old = axchg(&parent->child, NULL);
	ASSERT_NOT_NULL(old);
	ASSERT_EQ(count_tree_children(parent), 0);

	/* Free retired list */
	free_retired(old);

	/* Slots should be back on free list */
	ASSERT_NOT_NULL(aload(&(nodes + ino1)->next_free));
	ASSERT_NOT_NULL(aload(&(nodes + ino2)->next_free));

	return 0;
}

TEST(test_rebuild_preserves_live)
{
	struct inode *parent, *live, *old;
	unsigned long live_ino;

	parent = add_tree_node(get_tree_node(ROOT), "rebuild-parent",
	                       T_GENERIC, T_DIR);
	live = add_tree_node(parent, "live-node", T_GENERIC, T_FILE);
	add_tree_node(parent, "dead-node", T_GENERIC, T_FILE);

	/* kernel looked up live-node */
	tree_ref(live);
	live_ino = live->ino;

	/* Swap child list */
	old = axchg(&parent->child, NULL);
	free_retired(old);

	/* live-node should be detached, not freed */
	live = nodes + live_ino;
	ASSERT_EQ(aload(&live->flags) & INODE_DETACHED,
	          INODE_DETACHED);
	ASSERT_EQ(aload(&live->nlookup), 1);

	/* forgetting a detached node should reclaim it */
	tree_forget(live, 1);
	ASSERT_NOT_NULL(aload(&(nodes + live_ino)->next_free));

	return 0;
}

TEST(test_pool_grow)
{
	struct inode *parent, *n;
	char name[32];
	int i;

	parent = add_tree_node(get_tree_node(ROOT), "grow-parent",
	                       T_GENERIC, T_DIR);

	/* Allocate more nodes than initial batch to trigger growth */
	for (i = 0; i < 5000; i++) {
		snprintf(name, sizeof(name), "grow-%04d", i);
		n = add_tree_node(parent, name, T_GENERIC, T_FILE);
		ASSERT_NOT_NULL(n);
	}

	ASSERT_EQ(count_tree_children(parent), 5000);

	return 0;
}

TEST(test_deleted_in_sibling_walk)
{
	struct inode *parent, *b;

	parent = add_tree_node(get_tree_node(ROOT), "sib-walk",
	                       T_GENERIC, T_DIR);
	add_tree_node(parent, "sa", T_GENERIC, T_FILE);
	b = add_tree_node(parent, "sb", T_GENERIC, T_FILE);
	add_tree_node(parent, "sc", T_GENERIC, T_FILE);

	/* delete middle node */
	afor(&b->flags, INODE_DELETED);

	/* sibling walk should skip deleted nodes */
	ASSERT_EQ(count_tree_children(parent), 2);

	/* Verify both live nodes are reachable */
	ASSERT_NOT_NULL(get_tree_child(parent, "sa"));
	ASSERT_NOT_NULL(get_tree_child(parent, "sc"));

	return 0;
}

/* --- Error path tests --- */

TEST(test_get_tree_node_boundary)
{
	/*
	 * nnodes = TOP_INODES + TREE_SIZE (100007).
	 * Anything above that should return NULL.
	 */
	ASSERT_NULL(get_tree_node(200000));
	ASSERT_NULL(get_tree_node((unsigned long)-1));

	return 0;
}

TEST(test_add_node_has_ops)
{
	struct inode *parent, *child;

	parent = get_tree_node(ROOT);
	child = add_tree_node(parent, "test-ops", T_GENERIC, T_DIR);
	ASSERT_NOT_NULL(child);
	ASSERT_NOT_NULL(child->ops);

	/* Out-of-range type should give NULL ops */
	ASSERT_NULL(get_inode_ops(T_ALL));
	ASSERT_NULL(get_inode_ops(999));

	return 0;
}

TEST(test_tree_path_shallow)
{
	struct inode *parent, *tree_node;
	struct inode *tree_out = NULL;
	char path[512] = {0};

	/*
	 * tree_path on a T_TREE node whose parent is NOT T_TREE
	 * should return immediately with empty path and set tree_out
	 * to the node itself.
	 */
	parent = get_tree_node(HEAD);
	tree_node = add_tree_node(parent, "shallow-tree", T_TREE, T_DIR);

	tree_path(tree_node, path, sizeof(path), &tree_out);
	ASSERT_EQ((long)tree_out, (long)tree_node);
	ASSERT_STR_EQ(path, "");

	return 0;
}

TEST(test_count_children_after_dedup)
{
	struct inode *parent;

	parent = add_tree_node(get_tree_node(ROOT), "dedup-parent",
	                       T_GENERIC, T_DIR);

	add_tree_node(parent, "x", T_GENERIC, T_DIR);
	add_tree_node(parent, "y", T_GENERIC, T_DIR);

	/* Adding duplicates should not increase count */
	add_tree_node(parent, "x", T_GENERIC, T_DIR);
	add_tree_node(parent, "y", T_GENERIC, T_DIR);
	ASSERT_EQ(count_tree_children(parent), 2);

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

	/* Memory management tests */
	RUN_TEST(test_deleted_skip);
	RUN_TEST(test_ref_forget);
	RUN_TEST(test_forget_static);
	RUN_TEST(test_forget_detached);
	RUN_TEST(test_free_retired);
	RUN_TEST(test_rebuild_preserves_live);
	RUN_TEST(test_pool_grow);
	RUN_TEST(test_deleted_in_sibling_walk);

	TEST_SUMMARY();
}
