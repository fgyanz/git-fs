#include "harness.h"
#include "../inode.h"
#include "../tree.h"

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

	TEST_SUMMARY();
}
