#include "harness.h"
#include "../inode.h"
#include "../tree.h"

#include <git2.h>
#include <unistd.h>

static git_repository *repo;
static git_odb *test_odb;

/* what git-fs.c provides to inode.c, backed by the test repo */
git_odb *
get_gitfs_odb(void)
{
	if (test_odb)
		return test_odb;
	if (git_repository_odb(&test_odb, repo))
		return NULL;
	return test_odb;
}

git_repository *
get_gitfs_repo(void)
{
	return repo;
}

extern struct inode_ops *get_inode_ops(unsigned);

static int
run_cmd(const char *repo_path, const char *cmd)
{
	char buf[4096];

	snprintf(buf, sizeof(buf),
	         "cd '%s' && %s", repo_path, cmd);

	return system(buf);
}

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
	    run_cmd(p, "GIT_COMMITTER_DATE='@1700000000 +0000' "
	               "git commit -q -m 'first commit'") ||
	    run_cmd(p, "echo world > file2.txt") ||
	    run_cmd(p, "git add file2.txt") ||
	    run_cmd(p, "GIT_COMMITTER_DATE='@1700000100 +0000' "
	               "git commit -q -m 'second commit'") ||
	    run_cmd(p, "git tag v1.0") ||
	    run_cmd(p, "git branch feature")) {
		fprintf(stderr, "Failed to create test repo\n");
		return NULL;
	}

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
	if (system(cmd))
		fprintf(stderr, "failed to remove %s\n", path);
}

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
	git_oid_cpy(&n->oid, git_object_id(commit));
	n->mtime = git_commit_time((git_commit *) commit);
	git_object_free(commit);

	return n;
}

static git_time_t
commit_time_by_oid(const git_oid *oid)
{
	git_commit *c;
	git_time_t t;

	if (git_commit_lookup(&c, repo, oid))
		return 0;
	t = git_commit_time(c);
	git_commit_free(c);
	return t;
}

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
	ASSERT(count >= 2);

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
	ASSERT(n->mtime > 0);

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
	ASSERT(n->mtime > 0);

	n = ops->lookup(repo, tags, "no-such-tag");
	ASSERT_NULL(n);

	return 0;
}

TEST(test_update_commit)
{
	struct inode *head, *n;
	struct inode_ops *ops;

	head = head_commit_inode();
	ASSERT_NOT_NULL(head);

	ops = get_inode_ops(T_COMMIT);
	ASSERT_EQ(ops->update(repo, head), 0);

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

	/* names that only start with an entry's name must not match */
	n = ops->lookup(repo, head, "trees");
	ASSERT_NULL(n);

	n = ops->lookup(repo, head, "messages");
	ASSERT_NULL(n);

	n = ops->lookup(repo, head, "hashes");
	ASSERT_NULL(n);

	n = ops->lookup(repo, head, "parenting");
	ASSERT_NULL(n);

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

	ASSERT_EQ(ops->update(repo, head), 0);
	parent_node = get_tree_child(head, "parent");
	ASSERT_NOT_NULL(parent_node);
	ASSERT(parent_node->mtime > 0);

	root_commit = parent_node;
	ASSERT_EQ(ops->update(repo, root_commit), 0);

	return 0;
}

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

	tree_ops = get_inode_ops(T_TREE);
	ASSERT_EQ(tree_ops->update(repo, tree_node), 0);

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

	n = tree_ops->lookup(repo, tree_node, "no-such-file");
	ASSERT_NULL(n);

	return 0;
}

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
	ASSERT_EQ(n, 41);
	ASSERT_EQ(buf[40], '\n');

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

	n = tree_ops->lookup(repo, tree_node, "file.txt");
	ASSERT_NOT_NULL(n);

	ASSERT_EQ(n->size, 6);

	return 0;
}

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

	branch_ops = get_inode_ops(T_BRANCHES);
	ASSERT_EQ(branch_ops->update(repo, origin), 0);

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

	n = branch_ops->lookup(repo, origin, "feature");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->type, T_COMMIT);
	ASSERT(n->mtime > 0);

	n = branch_ops->lookup(repo, origin, "no-such-branch");
	ASSERT_NULL(n);

	return 0;
}

TEST(test_update_objects)
{
	struct inode *objects, *n;
	struct inode_ops *ops;
	size_t count;

	objects = get_tree_node(OBJECTS);
	ASSERT_NOT_NULL(objects);
	ASSERT_EQ(objects->type, T_OBJECTS);

	ops = get_inode_ops(T_OBJECTS);
	ASSERT_NOT_NULL(ops);
	ASSERT_NOT_NULL(ops->update);
	ASSERT_EQ(ops->update(repo, objects), 0);

	count = count_tree_children(objects);
	ASSERT(count >= 2);

	n = objects->child;
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->type, T_COMMIT);
	ASSERT_EQ(n->mode, T_DIR);
	ASSERT_EQ(strlen(n->name), 40);

	return 0;
}

TEST(test_lookup_objects)
{
	struct inode *objects, *n;
	struct inode_ops *ops;
	git_object *obj;
	char hex[GIT_OID_HEXSZ + 1];

	objects = get_tree_node(OBJECTS);
	ops = get_inode_ops(T_OBJECTS);

	if (git_revparse_single(&obj, repo, "HEAD"))
		FAIL("git_revparse_single failed");
	git_oid_tostr(hex, sizeof(hex), git_object_id(obj));
	git_object_free(obj);

	n = ops->lookup(repo, objects, hex);
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->type, T_COMMIT);
	ASSERT_EQ(n->mode, T_DIR);
	ASSERT(n->mtime > 0);
	ASSERT_STR_EQ(n->name, hex);

	return 0;
}

TEST(test_lookup_objects_invalid)
{
	struct inode *objects;
	struct inode_ops *ops;

	objects = get_tree_node(OBJECTS);
	ops = get_inode_ops(T_OBJECTS);

	ASSERT_NULL(ops->lookup(repo, objects, "not-a-hash"));

	ASSERT_NULL(ops->lookup(repo, objects, "abcdef"));

	ASSERT_NULL(ops->lookup(repo, objects,
		"0000000000000000000000000000000000000000"));

	ASSERT_NULL(ops->lookup(repo, objects, ""));

	return 0;
}

TEST(test_objects_commit_structure)
{
	struct inode *objects, *commit_node, *n;
	struct inode_ops *obj_ops, *commit_ops;
	git_object *obj;
	char hex[GIT_OID_HEXSZ + 1];

	objects = get_tree_node(OBJECTS);
	obj_ops = get_inode_ops(T_OBJECTS);

	if (git_revparse_single(&obj, repo, "HEAD"))
		FAIL("git_revparse_single failed");
	git_oid_tostr(hex, sizeof(hex), git_object_id(obj));
	git_object_free(obj);

	commit_node = obj_ops->lookup(repo, objects, hex);
	ASSERT_NOT_NULL(commit_node);

	commit_ops = get_inode_ops(T_COMMIT);
	ASSERT_EQ(commit_ops->update(repo, commit_node), 0);

	n = get_tree_child(commit_node, "tree");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->type, T_TREE);
	ASSERT_EQ(n->mode, T_DIR);

	n = get_tree_child(commit_node, "hash");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->type, T_HASH);
	ASSERT_EQ(n->mode, T_FILE);

	n = get_tree_child(commit_node, "msg");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->type, T_MSG);
	ASSERT_EQ(n->mode, T_FILE);

	n = get_tree_child(commit_node, "parent");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->type, T_COMMIT);
	ASSERT_EQ(n->mode, T_DIR);

	return 0;
}

TEST(test_objects_hash_matches_name)
{
	struct inode *objects, *commit_node, *hash_node;
	struct inode_ops *obj_ops, *commit_ops, *hash_ops;
	git_object *obj;
	char hex[GIT_OID_HEXSZ + 1];
	char buf[128] = {0};
	int fd;
	ssize_t n;

	objects = get_tree_node(OBJECTS);
	obj_ops = get_inode_ops(T_OBJECTS);

	if (git_revparse_single(&obj, repo, "HEAD"))
		FAIL("git_revparse_single failed");
	git_oid_tostr(hex, sizeof(hex), git_object_id(obj));
	git_object_free(obj);

	commit_node = obj_ops->lookup(repo, objects, hex);
	ASSERT_NOT_NULL(commit_node);

	commit_ops = get_inode_ops(T_COMMIT);
	ASSERT_EQ(commit_ops->update(repo, commit_node), 0);

	hash_node = get_tree_child(commit_node, "hash");
	ASSERT_NOT_NULL(hash_node);

	hash_ops = get_inode_ops(T_HASH);
	fd = hash_ops->open(repo, hash_node);
	ASSERT(fd >= 0);

	lseek(fd, 0, SEEK_SET);
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);

	ASSERT_EQ(n, 41);
	buf[40] = '\0';
	ASSERT_STR_EQ(buf, hex);

	return 0;
}

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

	ASSERT_NOT_NULL(get_tree_child(root_commit, "tree"));
	ASSERT_NOT_NULL(get_tree_child(root_commit, "hash"));
	ASSERT_NOT_NULL(get_tree_child(root_commit, "msg"));
	ASSERT_NULL(get_tree_child(root_commit, "parent"));

	return 0;
}

TEST(test_lookup_generic)
{
	struct inode *root, *n;
	struct inode_ops *ops;

	root = get_tree_node(ROOT);
	ops = get_inode_ops(T_GENERIC);

	/* static children: nothing to update */
	ASSERT_NULL(ops->update);

	n = ops->lookup(repo, root, "branches");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->ino, BRANCHES);

	n = ops->lookup(repo, root, "tags");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->ino, TAGS);

	n = ops->lookup(repo, root, "HEAD");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->ino, HEAD);

	n = ops->lookup(repo, root, "nope");
	ASSERT_NULL(n);

	return 0;
}

TEST(test_mtime_head)
{
	struct inode *head;
	struct inode_ops *ops;
	git_time_t expected;

	head = get_tree_node(HEAD);
	ASSERT_NOT_NULL(head);

	ops = get_inode_ops(T_HEAD);
	ASSERT_NOT_NULL(ops);
	ASSERT_NOT_NULL(ops->update);
	ASSERT_EQ(ops->update(repo, head), 0);

	expected = commit_time_by_oid(&head->oid);
	ASSERT(expected > 0);
	ASSERT_EQ(head->mtime, expected);

	return 0;
}

TEST(test_mtime_commit_children)
{
	struct inode *head, *n;
	struct inode_ops *ops;

	head = head_commit_inode();
	ASSERT_NOT_NULL(head);

	ops = get_inode_ops(T_COMMIT);
	ASSERT_EQ(ops->update(repo, head), 0);

	n = get_tree_child(head, "tree");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->mtime, head->mtime);

	n = get_tree_child(head, "hash");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->mtime, head->mtime);

	n = get_tree_child(head, "msg");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->mtime, head->mtime);

	return 0;
}

TEST(test_mtime_parent_own_time)
{
	struct inode *head, *parent;
	struct inode_ops *ops;
	git_time_t parent_time;

	head = head_commit_inode();
	ASSERT_NOT_NULL(head);

	ops = get_inode_ops(T_COMMIT);
	ASSERT_EQ(ops->update(repo, head), 0);

	parent = get_tree_child(head, "parent");
	ASSERT_NOT_NULL(parent);

	parent_time = commit_time_by_oid(&parent->oid);
	ASSERT(parent_time > 0);
	ASSERT_EQ(parent->mtime, parent_time);

	return 0;
}

TEST(test_mtime_branches)
{
	struct inode *heads, *n;
	struct inode_ops *ops;

	heads = get_tree_node(HEADS);
	ASSERT_NOT_NULL(heads);

	ops = get_inode_ops(T_BRANCHES);
	ASSERT_EQ(ops->update(repo, heads), 0);

	n = get_tree_child(heads, "feature");
	ASSERT_NOT_NULL(n);
	ASSERT(n->mtime > 0);
	ASSERT_EQ(n->mtime, commit_time_by_oid(&n->oid));

	return 0;
}

TEST(test_mtime_lookup_branches)
{
	struct inode *heads, *n;
	struct inode_ops *ops;

	heads = get_tree_node(HEADS);
	ops = get_inode_ops(T_BRANCHES);

	n = ops->lookup(repo, heads, "feature");
	ASSERT_NOT_NULL(n);
	ASSERT(n->mtime > 0);
	ASSERT_EQ(n->mtime, commit_time_by_oid(&n->oid));

	return 0;
}

TEST(test_mtime_tags)
{
	struct inode *tags, *n;
	struct inode_ops *ops;

	tags = get_tree_node(TAGS);
	ASSERT_NOT_NULL(tags);

	ops = get_inode_ops(T_TAGS);
	ASSERT_EQ(ops->update(repo, tags), 0);

	n = get_tree_child(tags, "v1.0");
	ASSERT_NOT_NULL(n);
	ASSERT(n->mtime > 0);
	ASSERT_EQ(n->mtime, commit_time_by_oid(&n->oid));

	return 0;
}

TEST(test_mtime_lookup_tags)
{
	struct inode *tags, *n;
	struct inode_ops *ops;

	tags = get_tree_node(TAGS);
	ops = get_inode_ops(T_TAGS);

	n = ops->lookup(repo, tags, "v1.0");
	ASSERT_NOT_NULL(n);
	ASSERT(n->mtime > 0);
	ASSERT_EQ(n->mtime, commit_time_by_oid(&n->oid));

	return 0;
}

TEST(test_mtime_tree_propagation)
{
	struct inode *head, *tree_node, *n;
	struct inode_ops *commit_ops, *tree_ops;

	head = head_commit_inode();
	ASSERT_NOT_NULL(head);

	commit_ops = get_inode_ops(T_COMMIT);
	ASSERT_EQ(commit_ops->update(repo, head), 0);

	tree_node = get_tree_child(head, "tree");
	ASSERT_NOT_NULL(tree_node);
	ASSERT_EQ(tree_node->mtime, head->mtime);

	tree_ops = get_inode_ops(T_TREE);
	ASSERT_EQ(tree_ops->update(repo, tree_node), 0);

	n = get_tree_child(tree_node, "file.txt");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->mtime, head->mtime);

	n = get_tree_child(tree_node, "file2.txt");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->mtime, head->mtime);

	return 0;
}

TEST(test_mtime_tree_lookup)
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
	ASSERT_EQ(n->mtime, head->mtime);

	return 0;
}

TEST(test_mtime_static_zero)
{
	struct inode *n;

	n = get_tree_node(ROOT);
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->mtime, 0);

	n = get_tree_node(BRANCHES);
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->mtime, 0);

	n = get_tree_node(TAGS);
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->mtime, 0);

	n = get_tree_node(REMOTES);
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->mtime, 0);

	return 0;
}

TEST(test_mtime_remote_branches)
{
	struct inode *remotes, *origin, *n;
	struct inode_ops *remote_ops, *branch_ops;

	remotes = get_tree_node(REMOTES);
	remote_ops = get_inode_ops(T_REMOTES);
	ASSERT_EQ(remote_ops->update(repo, remotes), 0);

	origin = get_tree_child(remotes, "origin");
	ASSERT_NOT_NULL(origin);

	branch_ops = get_inode_ops(T_BRANCHES);
	ASSERT_EQ(branch_ops->update(repo, origin), 0);

	n = origin->child;
	ASSERT_NOT_NULL(n);
	ASSERT(n->mtime > 0);
	ASSERT_EQ(n->mtime, commit_time_by_oid(&n->oid));

	return 0;
}

TEST(test_lookup_tree_lazy_obj)
{
	struct inode *head, *tree_node, *n;
	struct inode_ops *commit_ops, *tree_ops;
	int fd;

	head = head_commit_inode();
	ASSERT_NOT_NULL(head);

	commit_ops = get_inode_ops(T_COMMIT);
	ASSERT_EQ(commit_ops->update(repo, head), 0);

	tree_node = get_tree_child(head, "tree");
	ASSERT_NOT_NULL(tree_node);

	tree_ops = get_inode_ops(T_TREE);

	n = tree_ops->lookup(repo, tree_node, "file2.txt");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->mode, T_FILE);
	ASSERT(n->size > 0);

	ASSERT_NOT_NULL(tree_ops->open);
	fd = tree_ops->open(repo, n);
	ASSERT(fd >= 0);
	close(fd);

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

	dir = get_tree_child(tree_node, "tree");
	if (dir && dir->mode == T_DIR)
		ASSERT_EQ(dir->size, 0);

	return 0;
}

static size_t
live_inodes(void)
{
	struct tree_stats st;

	tree_stats(&st);
	return st.live;
}

static void
watcher_refresh(struct inode *p)
{
	struct inode *retired;

	free_retired(axchg(&p->retired, NULL));
	retired = axchg(&p->child, NULL);
	astore(&p->retired, retired);
	p->ops->update(repo, p);
}

TEST(test_repopulate_no_alloc)
{
	struct inode *head, *tree_node;
	size_t base = 0;
	int i;

	head = head_commit_inode();
	ASSERT_NOT_NULL(head);

	for (i = 0; i < 3; i++) {
		ASSERT_EQ(head->ops->update(repo, head), 0);
		tree_node = get_tree_child(head, "tree");
		ASSERT_NOT_NULL(tree_node);
		ASSERT_EQ(tree_node->ops->update(repo, tree_node), 0);
		ASSERT_NOT_NULL(tree_node->ops->lookup(repo, tree_node,
		                                       "file.txt"));
		if (i == 0)
			base = live_inodes();
	}
	ASSERT_EQ(live_inodes(), base);

	return 0;
}

TEST(test_refresh_cycles_bounded)
{
	struct inode *heads, *tags;
	size_t base = 0;
	int i;

	heads = get_tree_node(HEADS);
	tags = get_tree_node(TAGS);

	for (i = 0; i < 100; i++) {
		watcher_refresh(heads);
		watcher_refresh(tags);
		if (i == 1)
			base = live_inodes();
	}
	ASSERT_EQ(live_inodes(), base);

	return 0;
}

TEST(test_refresh_head_frees_subtree)
{
	struct inode *head, *n;
	size_t base;

	head = get_tree_node(HEAD);
	watcher_refresh(head);
	watcher_refresh(head);
	base = live_inodes();

	n = get_tree_child(head, "tree");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->ops->update(repo, n), 0);
	n = get_tree_child(head, "parent");
	ASSERT_NOT_NULL(n);
	ASSERT_EQ(n->ops->update(repo, n), 0);

	watcher_refresh(head);
	watcher_refresh(head);
	if (live_inodes() != base)
		FAIL("%zu inodes leaked", live_inodes() - base);

	return 0;
}

TEST(test_open_no_fd_leak)
{
	struct inode *head, *tree_node, *scratch, *bogus, *files[3];
	int i, j, fd, base;

	head = head_commit_inode();
	ASSERT_NOT_NULL(head);
	ASSERT_EQ(head->ops->update(repo, head), 0);
	tree_node = get_tree_child(head, "tree");
	ASSERT_NOT_NULL(tree_node);

	files[0] = get_tree_child(head, "hash");
	files[1] = get_tree_child(head, "msg");
	files[2] = tree_node->ops->lookup(repo, tree_node, "file.txt");
	for (j = 0; j < 3; j++)
		ASSERT_NOT_NULL(files[j]);

	scratch = add_tree_node(get_tree_node(ROOT), "fd-scratch",
	                        T_GENERIC, T_DIR, NULL, NULL);
	bogus = add_tree_node(scratch, "bogus", T_TREE, T_FILE, NULL, NULL);
	ASSERT_NOT_NULL(bogus);

	/* libgit2 opens pack and index files lazily: warm up first */
	for (j = 0; j < 3; j++) {
		fd = files[j]->ops->open(repo, files[j]);
		ASSERT(fd >= 0);
		close(fd);
	}
	base = count_open_fds();

	for (i = 0; i < 100; i++) {
		for (j = 0; j < 3; j++) {
			fd = files[j]->ops->open(repo, files[j]);
			ASSERT(fd >= 0);
			close(fd);
		}
		ASSERT_EQ(bogus->ops->open(repo, bogus), -1);
	}
	ASSERT_EQ(count_open_fds(), base);

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

	if (git_repository_open(&repo, repo_path)) {
		fprintf(stderr, "git_repository_open failed\n");
		goto out;
	}

	test_setup = tree_init;
	test_teardown = tree_destroy;

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
	RUN_TEST(test_update_objects);
	RUN_TEST(test_lookup_objects);
	RUN_TEST(test_lookup_objects_invalid);
	RUN_TEST(test_objects_commit_structure);
	RUN_TEST(test_objects_hash_matches_name);
	RUN_TEST(test_get_inode_ops_out_of_range);
	RUN_TEST(test_lookup_tree_nonexistent);
	RUN_TEST(test_lookup_commit_nonexistent);
	RUN_TEST(test_root_commit_no_parent_entry);
	RUN_TEST(test_node_size_directory);
	RUN_TEST(test_lookup_generic);
	RUN_TEST(test_mtime_head);
	RUN_TEST(test_mtime_commit_children);
	RUN_TEST(test_mtime_parent_own_time);
	RUN_TEST(test_mtime_branches);
	RUN_TEST(test_mtime_lookup_branches);
	RUN_TEST(test_mtime_tags);
	RUN_TEST(test_mtime_lookup_tags);
	RUN_TEST(test_mtime_tree_propagation);
	RUN_TEST(test_mtime_tree_lookup);
	RUN_TEST(test_mtime_static_zero);
	RUN_TEST(test_mtime_remote_branches);
	RUN_TEST(test_lookup_tree_lazy_obj);

	RUN_TEST(test_repopulate_no_alloc);
	RUN_TEST(test_refresh_cycles_bounded);
	RUN_TEST(test_open_no_fd_leak);
	RUN_XFAIL(test_refresh_head_frees_subtree,
	          "push_free() drops the subtree of a retired node");

	if (test_odb)
		git_odb_free(test_odb);
	git_repository_free(repo);
out:
	cleanup_test_repo(repo_path);
	git_libgit2_shutdown();

	TEST_SUMMARY();
}
