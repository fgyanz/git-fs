#include "harness.h"
#include "../inode.h"
#include "../tree.h"

#include <git2.h>
#include <unistd.h>

static git_repository *repo;
static git_odb *test_odb;

/* stub for inode.c's get_gitfs_odb() — uses the test repo */
git_odb *
get_gitfs_odb(void)
{
	if (test_odb)
		return test_odb;
	if (git_repository_odb(&test_odb, repo))
		return NULL;
	return test_odb;
}

/* Declared in inode.c */
extern struct inode_ops *get_inode_ops(unsigned);

/*
 * Helper: run a shell command in the test repo directory.
 * Returns 0 on success.
 */
static int
run_cmd(const char *repo_path, const char *cmd)
{
	char buf[4096];

	snprintf(buf, sizeof(buf),
	         "cd '%s' && %s", repo_path, cmd);

	return system(buf);
}

/*
 * Create a temporary git repo with known state:
 *   - Two commits ("first commit", "second commit")
 *   - file.txt with "hello\n" (first commit)
 *   - file2.txt with "world\n" (second commit)
 *   - Tag v1.0 on second commit
 *   - Branch "feature" on second commit
 *   - A bare clone as "origin" remote with tracking branches
 */
static char repo_path[] = "/tmp/git-fs-test-XXXXXX";
static char bare_path[] = "/tmp/git-fs-bare-XXXXXX";

static char *
create_test_repo(void)
{
	char *p;
	char cmd[4096];

	p = mkdtemp(repo_path);
	if (!p) {
		perror("mkdtemp");
		return NULL;
	}

	if (run_cmd(p, "git init -q") ||
	    run_cmd(p, "git config user.email test@test") ||
	    run_cmd(p, "git config user.name test") ||
	    run_cmd(p, "echo hello > file.txt") ||
	    run_cmd(p, "git add file.txt") ||
	    run_cmd(p, "git commit -q -m 'first commit'") ||
	    run_cmd(p, "echo world > file2.txt") ||
	    run_cmd(p, "git add file2.txt") ||
	    run_cmd(p, "git commit -q -m 'second commit'") ||
	    run_cmd(p, "git tag v1.0") ||
	    run_cmd(p, "git branch feature")) {
		fprintf(stderr, "Failed to create test repo\n");
		return NULL;
	}

	/* Create bare clone and add as remote for tracking branches */
	if (!mkdtemp(bare_path)) {
		perror("mkdtemp bare");
		return NULL;
	}

	snprintf(cmd, sizeof(cmd),
	         "git clone -q --bare '%s' '%s/repo.git' && "
	         "cd '%s' && "
	         "git remote add origin '%s/repo.git' && "
	         "git fetch -q origin",
	         p, bare_path, p, bare_path);

	if (system(cmd)) {
		fprintf(stderr, "Failed to set up remote\n");
		return NULL;
	}

	return p;
}

static void
cleanup_test_repo(const char *path)
{
	char cmd[4096];

	snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s'", path, bare_path);
	system(cmd);
}

/*
 * Helper: get a commit inode for HEAD, with obj populated.
 */
static struct inode *
head_commit_inode(void)
{
	struct inode *n;
	git_object *obj, *commit;

	n = get_tree_node(HEAD);
	if (!n)
		return NULL;

	if (git_revparse_single(&obj, repo, "HEAD"))
		return NULL;

	if (git_object_peel(&commit, obj, GIT_OBJECT_COMMIT)) {
		git_object_free(obj);
		return NULL;
	}

	git_object_free(obj);
	n->obj = commit;

	return n;
}

/* --- Tree structure tests (branches, tags, remotes) --- */

TEST(test_update_branches)
{
	struct inode *heads, *n;
	struct inode_ops *ops;
	size_t count;

	heads = get_tree_node(HEADS);
	ASSERT_NOT_NULL(heads);

	ops = get_inode_ops(T_BRANCHES);
	ASSERT_NOT_NULL(ops);
	ASSERT_EQ(ops->update(repo, heads), 0);

	count = count_tree_children(heads);
	/* At least "master"/"main" and "feature" */
	ASSERT(count >= 2);

	/* "feature" branch should exist */
	n = get_tree_child(heads, "feature");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->type, T_COMMIT);
	ASSERT_EQ(n->mode, T_DIR);

	return 0;
}

TEST(test_lookup_branches)
{
	struct inode *heads, *n;
	struct inode_ops *ops;

	heads = get_tree_node(HEADS);
	ops = get_inode_ops(T_BRANCHES);

	n = ops->lookup(repo, heads, "feature");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->type, T_COMMIT);
	ASSERT_NOT_NULL(n->obj);

	/* Non-existent branch */
	n = ops->lookup(repo, heads, "no-such-branch");
	ASSERT_NULL(n);

	return 0;
}

TEST(test_update_tags)
{
	struct inode *tags;
	struct inode_ops *ops;
	struct inode *n;

	tags = get_tree_node(TAGS);
	ASSERT_NOT_NULL(tags);

	ops = get_inode_ops(T_TAGS);
	ASSERT_EQ(ops->update(repo, tags), 0);

	n = get_tree_child(tags, "v1.0");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->type, T_COMMIT);

	return 0;
}

TEST(test_lookup_tags)
{
	struct inode *tags, *n;
	struct inode_ops *ops;

	tags = get_tree_node(TAGS);
	ops = get_inode_ops(T_TAGS);

	n = ops->lookup(repo, tags, "v1.0");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->type, T_COMMIT);
	ASSERT_NOT_NULL(n->obj);

	n = ops->lookup(repo, tags, "no-such-tag");
	ASSERT_NULL(n);

	return 0;
}

/* --- Commit tests --- */

TEST(test_update_commit)
{
	struct inode *head, *n;
	struct inode_ops *ops;

	head = head_commit_inode();
	ASSERT_NOT_NULL(head);

	ops = get_inode_ops(T_COMMIT);
	ASSERT_EQ(ops->update(repo, head), 0);

	/* Should have: tree, hash, msg, parent */
	n = get_tree_child(head, "tree");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->type, T_TREE);
	ASSERT_EQ(n->mode, T_DIR);

	n = get_tree_child(head, "hash");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->type, T_HASH);
	ASSERT_EQ(n->mode, T_FILE);

	n = get_tree_child(head, "msg");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->type, T_MSG);
	ASSERT_EQ(n->mode, T_FILE);

	n = get_tree_child(head, "parent");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->type, T_COMMIT);
	ASSERT_EQ(n->mode, T_DIR);

	return 0;
}

TEST(test_lookup_commit_exact)
{
	struct inode *head, *n;
	struct inode_ops *ops;

	head = head_commit_inode();
	ASSERT_NOT_NULL(head);

	ops = get_inode_ops(T_COMMIT);

	n = ops->lookup(repo, head, "tree");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->type, T_TREE);

	n = ops->lookup(repo, head, "hash");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->type, T_HASH);

	n = ops->lookup(repo, head, "msg");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->type, T_MSG);

	n = ops->lookup(repo, head, "parent");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->type, T_COMMIT);

	return 0;
}

TEST(test_lookup_commit_prefix_rejected)
{
	struct inode *head, *n;
	struct inode_ops *ops;

	head = head_commit_inode();
	ASSERT_NOT_NULL(head);

	ops = get_inode_ops(T_COMMIT);

	/*
	 * The strncmp bug: lookup_commit uses strncmp with strlen(d->name)
	 * as the length. Short prefixes ("tr") are rejected by strncmp
	 * because the null in "tr" differs from 'e' in "tree". But longer
	 * strings that START with a dentry name should NOT match either.
	 * e.g., "trees" should not match "tree".
	 */
	n = ops->lookup(repo, head, "trees");
	ASSERT_NULL(n);

	n = ops->lookup(repo, head, "messages");
	ASSERT_NULL(n);

	n = ops->lookup(repo, head, "hashes");
	ASSERT_NULL(n);

	n = ops->lookup(repo, head, "parenting");
	ASSERT_NULL(n);

	/* Non-existent entry */
	n = ops->lookup(repo, head, "nonexistent");
	ASSERT_NULL(n);

	return 0;
}

TEST(test_root_commit_parent)
{
	struct inode *head, *parent_node, *root_commit;
	struct inode_ops *ops;

	head = head_commit_inode();
	ASSERT_NOT_NULL(head);

	ops = get_inode_ops(T_COMMIT);

	/* Navigate to parent (first commit = root commit) */
	ASSERT_EQ(ops->update(repo, head), 0);
	parent_node = get_tree_child(head, "parent");
	ASSERT_NOT_NULL(parent_node);
	ASSERT_NOT_NULL(parent_node->obj);

	/*
	 * The root commit has no parent. update_commit on the root
	 * commit should NOT fail — it should gracefully handle the
	 * missing parent (e.g., skip or create an empty parent entry).
	 */
	root_commit = parent_node;
	ASSERT_EQ(ops->update(repo, root_commit), 0);

	return 0;
}

/* --- Tree (file listing) tests --- */

TEST(test_update_tree)
{
	struct inode *head, *tree_node, *n;
	struct inode_ops *commit_ops, *tree_ops;

	head = head_commit_inode();
	ASSERT_NOT_NULL(head);

	commit_ops = get_inode_ops(T_COMMIT);
	ASSERT_EQ(commit_ops->update(repo, head), 0);

	tree_node = get_tree_child(head, "tree");
	ASSERT_NOT_NULL(tree_node);
	ASSERT_NOT_NULL(tree_node->obj);

	tree_ops = get_inode_ops(T_TREE);
	ASSERT_EQ(tree_ops->update(repo, tree_node), 0);

	/* Should contain file.txt and file2.txt with sizes set */
	n = get_tree_child(tree_node, "file.txt");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->mode, T_FILE);
	ASSERT(n->size > 0);

	n = get_tree_child(tree_node, "file2.txt");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->mode, T_FILE);
	ASSERT(n->size > 0);

	return 0;
}

TEST(test_lookup_tree)
{
	struct inode *head, *tree_node, *n;
	struct inode_ops *commit_ops, *tree_ops;

	head = head_commit_inode();
	ASSERT_NOT_NULL(head);

	commit_ops = get_inode_ops(T_COMMIT);
	ASSERT_EQ(commit_ops->update(repo, head), 0);

	tree_node = get_tree_child(head, "tree");
	ASSERT_NOT_NULL(tree_node);

	tree_ops = get_inode_ops(T_TREE);

	n = tree_ops->lookup(repo, tree_node, "file.txt");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->mode, T_FILE);
	ASSERT(n->size > 0);

	/* Non-existent */
	n = tree_ops->lookup(repo, tree_node, "no-such-file");
	ASSERT_NULL(n);

	return 0;
}

/* --- File open tests --- */

TEST(test_open_hash)
{
	struct inode *head, *hash_node;
	struct inode_ops *commit_ops, *hash_ops;
	int fd;
	char buf[128] = {0};
	ssize_t n;

	head = head_commit_inode();
	ASSERT_NOT_NULL(head);

	commit_ops = get_inode_ops(T_COMMIT);
	ASSERT_EQ(commit_ops->update(repo, head), 0);

	hash_node = get_tree_child(head, "hash");
	ASSERT_NOT_NULL(hash_node);

	hash_ops = get_inode_ops(T_HASH);
	ASSERT_NOT_NULL(hash_ops->open);

	fd = hash_ops->open(repo, hash_node);
	ASSERT(fd >= 0);

	lseek(fd, 0, SEEK_SET);
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);

	ASSERT(n > 0);
	/* Should be 40 hex chars + newline */
	ASSERT_EQ(n, 41);
	ASSERT_EQ(buf[40], '\n');

	/* Verify it's valid hex */
	for (int i = 0; i < 40; i++) {
		char c = buf[i];
		ASSERT((c >= '0' && c <= '9') ||
		       (c >= 'a' && c <= 'f'));
	}

	return 0;
}

TEST(test_open_msg)
{
	struct inode *head, *msg_node;
	struct inode_ops *commit_ops, *msg_ops;
	int fd;
	char buf[4096] = {0};
	ssize_t n;

	head = head_commit_inode();
	ASSERT_NOT_NULL(head);

	commit_ops = get_inode_ops(T_COMMIT);
	ASSERT_EQ(commit_ops->update(repo, head), 0);

	msg_node = get_tree_child(head, "msg");
	ASSERT_NOT_NULL(msg_node);

	msg_ops = get_inode_ops(T_MSG);
	ASSERT_NOT_NULL(msg_ops->open);

	fd = msg_ops->open(repo, msg_node);
	ASSERT(fd >= 0);

	lseek(fd, 0, SEEK_SET);
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);

	ASSERT(n > 0);
	/* Should contain "second commit" */
	ASSERT(strstr(buf, "second commit") != NULL);

	return 0;
}

TEST(test_open_blob)
{
	struct inode *head, *tree_node, *file_node;
	struct inode_ops *commit_ops, *tree_ops;
	int fd;
	char buf[4096] = {0};
	ssize_t n;

	head = head_commit_inode();
	ASSERT_NOT_NULL(head);

	commit_ops = get_inode_ops(T_COMMIT);
	ASSERT_EQ(commit_ops->update(repo, head), 0);

	tree_node = get_tree_child(head, "tree");
	ASSERT_NOT_NULL(tree_node);

	tree_ops = get_inode_ops(T_TREE);

	file_node = tree_ops->lookup(repo, tree_node, "file.txt");
	ASSERT_NOT_NULL(file_node);
	ASSERT_NOT_NULL(tree_ops->open);

	fd = tree_ops->open(repo, file_node);
	ASSERT(fd >= 0);

	lseek(fd, 0, SEEK_SET);
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);

	ASSERT(n > 0);
	ASSERT_STR_EQ(buf, "hello\n");

	return 0;
}

TEST(test_get_node_size)
{
	struct inode *head, *tree_node, *n;
	struct inode_ops *commit_ops, *tree_ops;

	head = head_commit_inode();
	ASSERT_NOT_NULL(head);

	commit_ops = get_inode_ops(T_COMMIT);
	ASSERT_EQ(commit_ops->update(repo, head), 0);

	tree_node = get_tree_child(head, "tree");
	ASSERT_NOT_NULL(tree_node);

	tree_ops = get_inode_ops(T_TREE);

	/* Lookup file to get size populated */
	n = tree_ops->lookup(repo, tree_node, "file.txt");
	ASSERT_NOT_NULL(n);

	/* "hello\n" = 6 bytes */
	ASSERT_EQ(n->size, 6);

	return 0;
}

/* --- Remotes tests --- */

TEST(test_update_remotes)
{
	struct inode *remotes, *n;
	struct inode_ops *ops;

	remotes = get_tree_node(REMOTES);
	ASSERT_NOT_NULL(remotes);

	ops = get_inode_ops(T_REMOTES);
	ASSERT_NOT_NULL(ops);
	ASSERT_EQ(ops->update(repo, remotes), 0);

	n = get_tree_child(remotes, "origin");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->type, T_BRANCHES);
	ASSERT_EQ(n->mode, T_DIR);

	return 0;
}

TEST(test_lookup_remotes)
{
	struct inode *remotes, *n;
	struct inode_ops *ops;

	remotes = get_tree_node(REMOTES);
	ops = get_inode_ops(T_REMOTES);

	n = ops->lookup(repo, remotes, "origin");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->type, T_BRANCHES);

	n = ops->lookup(repo, remotes, "no-such-remote");
	ASSERT_NULL(n);

	return 0;
}

TEST(test_update_remote_branches)
{
	struct inode *remotes, *origin, *n;
	struct inode_ops *remote_ops, *branch_ops;

	remotes = get_tree_node(REMOTES);
	remote_ops = get_inode_ops(T_REMOTES);
	ASSERT_EQ(remote_ops->update(repo, remotes), 0);

	origin = get_tree_child(remotes, "origin");
	ASSERT_NOT_NULL(origin);

	/* origin is a T_BRANCHES node under T_REMOTES parent */
	branch_ops = get_inode_ops(T_BRANCHES);
	ASSERT_EQ(branch_ops->update(repo, origin), 0);

	/* Should have the default branch as a remote tracking branch */
	n = origin->child;
	ASSERT_NOT_NULL(n);
	ASSERT(count_tree_children(origin) >= 1);

	return 0;
}

TEST(test_lookup_remote_branches)
{
	struct inode *remotes, *origin, *n;
	struct inode_ops *remote_ops, *branch_ops;

	remotes = get_tree_node(REMOTES);
	remote_ops = get_inode_ops(T_REMOTES);
	ASSERT_EQ(remote_ops->update(repo, remotes), 0);

	origin = get_tree_child(remotes, "origin");
	ASSERT_NOT_NULL(origin);

	branch_ops = get_inode_ops(T_BRANCHES);

	/* "feature" was pushed to the bare clone */
	n = branch_ops->lookup(repo, origin, "feature");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->type, T_COMMIT);
	ASSERT_NOT_NULL(n->obj);

	n = branch_ops->lookup(repo, origin, "no-such-branch");
	ASSERT_NULL(n);

	return 0;
}

/* --- Error path tests --- */

TEST(test_get_inode_ops_out_of_range)
{
	ASSERT_NULL(get_inode_ops(T_ALL));
	ASSERT_NULL(get_inode_ops(T_ALL + 1));
	ASSERT_NULL(get_inode_ops(999));

	return 0;
}

TEST(test_lookup_tree_nonexistent)
{
	struct inode *head, *tree_node, *n;
	struct inode_ops *commit_ops, *tree_ops;

	head = head_commit_inode();
	ASSERT_NOT_NULL(head);

	commit_ops = get_inode_ops(T_COMMIT);
	ASSERT_EQ(commit_ops->update(repo, head), 0);

	tree_node = get_tree_child(head, "tree");
	ASSERT_NOT_NULL(tree_node);

	tree_ops = get_inode_ops(T_TREE);

	/* Various non-existent paths */
	n = tree_ops->lookup(repo, tree_node, "");
	ASSERT_NULL(n);

	n = tree_ops->lookup(repo, tree_node, "no-such-file.txt");
	ASSERT_NULL(n);

	n = tree_ops->lookup(repo, tree_node, "..");
	ASSERT_NULL(n);

	return 0;
}

TEST(test_lookup_commit_nonexistent)
{
	struct inode *head, *n;
	struct inode_ops *ops;

	head = head_commit_inode();
	ASSERT_NOT_NULL(head);

	ops = get_inode_ops(T_COMMIT);

	n = ops->lookup(repo, head, "");
	ASSERT_NULL(n);

	n = ops->lookup(repo, head, "nonexistent");
	ASSERT_NULL(n);

	return 0;
}

TEST(test_root_commit_no_parent_entry)
{
	struct inode *head, *parent_node, *root_commit;
	struct inode_ops *ops;

	head = head_commit_inode();
	ASSERT_NOT_NULL(head);

	ops = get_inode_ops(T_COMMIT);
	ASSERT_EQ(ops->update(repo, head), 0);

	parent_node = get_tree_child(head, "parent");
	ASSERT_NOT_NULL(parent_node);

	root_commit = parent_node;
	ASSERT_EQ(ops->update(repo, root_commit), 0);

	/* Root commit should have tree/hash/msg but no parent */
	ASSERT_NOT_NULL(get_tree_child(root_commit, "tree"));
	ASSERT_NOT_NULL(get_tree_child(root_commit, "hash"));
	ASSERT_NOT_NULL(get_tree_child(root_commit, "msg"));
	ASSERT_NULL(get_tree_child(root_commit, "parent"));

	return 0;
}

TEST(test_update_generic)
{
	struct inode *root, *head;
	struct inode_ops *ops;

	root = get_tree_node(ROOT);
	ASSERT_NOT_NULL(root);

	ops = get_inode_ops(T_GENERIC);
	ASSERT_NOT_NULL(ops);
	ASSERT_EQ(ops->update(repo, root), 0);

	/* update_generic on ROOT should populate HEAD's obj */
	head = get_tree_node(HEAD);
	ASSERT_NOT_NULL(head);
	ASSERT_NOT_NULL(head->obj);

	return 0;
}

TEST(test_lookup_generic)
{
	struct inode *root, *n;
	struct inode_ops *ops;

	root = get_tree_node(ROOT);
	ops = get_inode_ops(T_GENERIC);

	/* should find static children */
	n = ops->lookup(repo, root, "branches");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->ino, BRANCHES);

	n = ops->lookup(repo, root, "tags");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->ino, TAGS);

	/* HEAD lookup should also populate its obj */
	n = ops->lookup(repo, root, "HEAD");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->ino, HEAD);
	ASSERT_NOT_NULL(n->obj);

	/* non-existent */
	n = ops->lookup(repo, root, "nope");
	ASSERT_NULL(n);

	return 0;
}

TEST(test_node_size_directory)
{
	struct inode *head, *tree_node;
	struct inode_ops *commit_ops, *tree_ops;
	struct inode *dir;

	head = head_commit_inode();
	ASSERT_NOT_NULL(head);

	commit_ops = get_inode_ops(T_COMMIT);
	ASSERT_EQ(commit_ops->update(repo, head), 0);

	tree_node = get_tree_child(head, "tree");
	ASSERT_NOT_NULL(tree_node);

	tree_ops = get_inode_ops(T_TREE);
	ASSERT_EQ(tree_ops->update(repo, tree_node), 0);

	/* Directories should have size 0 */
	dir = get_tree_child(tree_node, "tree");
	if (dir && dir->mode == T_DIR)
		ASSERT_EQ(dir->size, 0);

	return 0;
}

int
main(void)
{
	char *repo_path;

	fprintf(stderr, "test_inode:\n");

	git_libgit2_init();

	repo_path = create_test_repo();
	if (!repo_path)
		return EXIT_FAILURE;

	if (tree_init()) {
		fprintf(stderr, "tree_init failed\n");
		goto out;
	}

	if (git_repository_open(&repo, repo_path)) {
		fprintf(stderr, "git_repository_open failed\n");
		goto out;
	}

	RUN_TEST(test_update_branches);
	RUN_TEST(test_lookup_branches);
	RUN_TEST(test_update_tags);
	RUN_TEST(test_lookup_tags);
	RUN_TEST(test_update_commit);
	RUN_TEST(test_lookup_commit_exact);
	RUN_TEST(test_lookup_commit_prefix_rejected);
	RUN_TEST(test_root_commit_parent);
	RUN_TEST(test_update_tree);
	RUN_TEST(test_lookup_tree);
	RUN_TEST(test_open_hash);
	RUN_TEST(test_open_msg);
	RUN_TEST(test_open_blob);
	RUN_TEST(test_get_node_size);
	RUN_TEST(test_update_remotes);
	RUN_TEST(test_lookup_remotes);
	RUN_TEST(test_update_remote_branches);
	RUN_TEST(test_lookup_remote_branches);
	RUN_TEST(test_get_inode_ops_out_of_range);
	RUN_TEST(test_lookup_tree_nonexistent);
	RUN_TEST(test_lookup_commit_nonexistent);
	RUN_TEST(test_root_commit_no_parent_entry);
	RUN_TEST(test_node_size_directory);
	RUN_TEST(test_update_generic);
	RUN_TEST(test_lookup_generic);

	if (test_odb)
		git_odb_free(test_odb);
	git_repository_free(repo);
out:
	cleanup_test_repo(repo_path);
	git_libgit2_shutdown();

	TEST_SUMMARY();
}
