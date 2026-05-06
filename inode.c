#include <errno.h>
#include <git2.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "inode.h"
#include "tree.h"

#define ARRAY_SIZE(x)  (sizeof(x) / sizeof(x[0]))

static inline void
set_obj(struct inode *n, struct git_object *obj)
{
	struct git_object *old;

	old = axchg(&n->obj, obj);
	if (old && old != obj)
		git_object_free(old);
}

/*
 * add_tree_node init callback: save the oid on the fresh inode before publish
 */
static void
set_oid(struct inode *n, void *arg)
{
	git_oid_cpy(&n->oid, arg);
}

void
inode_release(struct inode *n)
{
	struct git_object *obj;

	free(n->name);
	n->name = NULL;

	obj = axchg(&n->obj, NULL);
	if (obj)
		git_object_free(obj);
}

static inline void
set_commit(struct inode *n, git_object *obj)
{
	set_obj(n, obj);
	n->mtime = git_commit_time((git_commit *) obj);
}

#define GIT_BRANCH(x)  (is_remote(x) ? \
                        GIT_BRANCH_REMOTE : GIT_BRANCH_LOCAL)

#define GIT_HASH_SZ    (GIT_OID_HEXSZ + 1)

struct hardcoded_dentry {
	const char *name;
	unsigned type;
	mode_t mode;
};

/* the four virtual children every commit-like dir (T_COMMIT, T_HEAD)
 * exposes.  */
static struct hardcoded_dentry dentries[] = {
	{"tree",   T_TREE,   T_DIR},
	{"hash",   T_HASH,   T_FILE},
	{"msg",    T_MSG,    T_FILE},
	{"parent", T_COMMIT, T_DIR},
};

static inline int
is_dentry_dir(const git_tree_entry *dentry)
{
	return git_tree_entry_type(dentry) == GIT_OBJECT_TREE;
}

static inline int
is_remote(struct inode *n)
{
	return n->parent->type == T_REMOTES;
}

extern git_odb *get_gitfs_odb(void);

static size_t
get_node_size(struct inode *node)
{
	git_odb *odb;
	size_t size = 0;
	git_otype type;

	if (node->type == T_HASH)
		return GIT_HASH_SZ;

	if (node->type == T_MSG)
		return strlen(git_commit_message((git_commit *)node->obj));

	if (node->mode == T_DIR)
		return 0;

	odb = get_gitfs_odb();
	if (!odb)
		return 0;

	/* use stored oid; does not load the file into memory */
	if (git_odb_read_header(&size, &type, odb, &node->oid))
		return 0;

	return size;
}

static int
resolve_commit(struct inode *n, struct inode *dir, unsigned type)
{
	git_commit *c;
	git_tree *t;
	git_object *dup;

	switch (type) {
	case T_PARENT:
		if (git_commit_parent(&c, (git_commit *) dir->obj, 0))
			return 1;
		set_commit(n, (git_object *) c);
		break;
	case T_TREE:
		if (git_commit_tree(&t, (git_commit *) dir->obj))
			return 1;
		set_obj(n, (git_object *) t);
		n->mtime = dir->mtime;
		break;
	default:
		git_object_dup(&dup, dir->obj);
		set_obj(n, dup);
		n->mtime = dir->mtime;
	}
	return 0;
}

static int
resolve_commit_obj(git_repository *repo, struct inode *n)
{
	git_object *obj, *expected;

	if (aload(&n->obj))
		return 0;

	if (git_object_lookup(&obj, repo, &n->oid, GIT_OBJECT_COMMIT))
		return 1;

	expected = NULL;
	if (!acas(&n->obj, &expected, obj))
		git_object_free(obj);

	return 0;
}

static int
update_commit(git_repository *repo, struct inode *dir)
{
	size_t i;
	struct inode *n;
	struct hardcoded_dentry *d;

	if (resolve_commit_obj(repo, dir))
		return 1;

	if (!dir->mtime)
		dir->mtime = git_commit_time((git_commit *) dir->obj);

	for (i = 0; i < ARRAY_SIZE(dentries); i++) {
		d = dentries + i;

		/* root commits have no parent */
		if (d->type == T_PARENT &&
		    git_commit_parentcount((git_commit *) dir->obj) == 0)
			continue;

		n = add_tree_node(dir, d->name, d->type, d->mode, NULL, NULL);
		if (!n)
			return 1;
		if (resolve_commit(n, dir, d->type))
			return 1;
		n->size = get_node_size(n);
	}

	return 0;
}

static struct inode *
lookup_commit(git_repository *repo, struct inode *dir, const char *entry)
{
	size_t i;
	struct hardcoded_dentry *d;
	struct inode *n;

	d = NULL;
	for (i = 0; i < ARRAY_SIZE(dentries); i++) {
		d = dentries + i;
		if (strcmp(d->name, entry) == 0)
			break;
		d = NULL;
	}

	if (!d)
		return NULL;

	if (resolve_commit_obj(repo, dir))
		return NULL;

	if (!dir->mtime)
		dir->mtime = git_commit_time((git_commit *) dir->obj);

	n = add_tree_node(dir, d->name, d->type, d->mode, NULL, NULL);
	if (!n)
		return NULL;
	if (resolve_commit(n, dir, d->type))
		return NULL;

	n->size = get_node_size(n);

	return n;
}

static int
update_tags(git_repository *repo, struct inode *dir)
{
	git_reference_iterator *it;
	git_reference *r;
	git_object *obj;
	char *tag;
	struct inode *n;

	if (git_reference_iterator_glob_new(&it, repo, "refs/tags/*"))
		return 1;

	while (git_reference_next(&r, it) == 0) {
		if (git_reference_peel(&obj, r, GIT_OBJECT_COMMIT))
			continue;
		tag = (char *) git_reference_name(r);
		n = add_tree_node(dir, basename(tag), T_COMMIT, T_DIR, NULL, NULL);
		if (n)
			set_commit(n, obj);
	}

	git_reference_iterator_free(it);
	return 0;
}

static struct inode *
lookup_tags(git_repository *repo, struct inode *dir, const char *entry)
{
	git_reference *r;
	git_object *obj;
	char ref_path[PATH_MAX];
	struct inode *n;

	snprintf(ref_path, sizeof(ref_path), "refs/tags/%s", entry);

	if (git_reference_lookup(&r, repo, ref_path))
		return NULL;

	if (git_reference_peel(&obj, r, GIT_OBJECT_COMMIT)) {
		git_reference_free(r);
		return NULL;
	}

	git_reference_free(r);
	n = add_tree_node(dir, entry, T_COMMIT, T_DIR, NULL, NULL);
	if (!n)
		return NULL;
	set_commit(n, obj);

	return n;
}

static int
update_remotes(git_repository *repo, struct inode *dir)
{
	size_t i;
	git_strarray remotes = {0};

	if (git_remote_list(&remotes, repo))
		return 1;

	for (i = 0; i < remotes.count; i++)
		add_tree_node(dir, remotes.strings[i], T_BRANCHES, T_DIR, NULL, NULL);

	git_strarray_dispose(&remotes);

	return 0;
}

static struct inode *
lookup_remotes(git_repository *repo, struct inode *dir, const char *entry)
{
	git_remote *r;

	if (git_remote_lookup(&r, repo, entry))
		return NULL;

	git_remote_free(r);
	return add_tree_node(dir, entry, T_BRANCHES, T_DIR, NULL, NULL);
}

static int
update_branches(git_repository *repo, struct inode *dir)
{
	git_branch_iterator *it;
	git_reference *r;
	git_branch_t br_type;
	git_object *obj;
	const char *br;
	size_t l;
	char *remote;
	struct inode *n;

	l = 0;
	remote = NULL;

	if (is_remote(dir)) {
		remote = dir->name;
		l = strlen(remote);
	}

	if (git_branch_iterator_new(&it, repo, GIT_BRANCH(dir)))
		return 1;

	while (git_branch_next(&r, &br_type, it) == 0) {
		git_branch_name(&br, r);
		if (!br)
			continue;

		if (git_reference_peel(&obj, r, GIT_OBJECT_COMMIT))
			continue;

		if (remote) {
			if (strncmp(br, remote, l) != 0 || br[l] != '/')
				continue;
			br = br + l + 1;
		}

		/* multi-level branch names not supported yet */
		if (strchr(br, '/'))
			continue;

		n = add_tree_node(dir, br, T_COMMIT, T_DIR, NULL, NULL);
		if (n)
			set_commit(n, obj);
	}

	git_branch_iterator_free(it);

	return 0;
}

static struct inode *
lookup_branches(git_repository *repo, struct inode *dir, const char *entry)
{
	git_reference *r;
	git_object *obj;
	struct inode *n;
	char remote[PATH_MAX];
	const char *br;

	if (is_remote(dir)) {
		snprintf(remote, sizeof(remote), "%s/%s", dir->name, entry);
		br = remote;
	} else {
		br = entry;
	}

	if (git_branch_lookup(&r, repo, br, GIT_BRANCH(dir)))
		return NULL;

	if (git_reference_peel(&obj, r, GIT_OBJECT_COMMIT)) {
		git_reference_free(r);
		return NULL;
	}

	git_reference_free(r);
	n = add_tree_node(dir, entry, T_COMMIT, T_DIR, NULL, NULL);
	if (!n)
		return NULL;
	set_commit(n, obj);

	return n;
}

static int
resolve_tree_obj(git_repository *repo, struct inode *n)
{
	git_object *obj, *expected;

	if (aload(&n->obj))
		return 0;

	if (git_object_lookup(&obj, repo, &n->oid, GIT_OBJECT_ANY))
		return 1;

	expected = NULL;
	if (!acas(&n->obj, &expected, obj))
		git_object_free(obj);

	return 0;
}

static int
update_tree(git_repository *repo, struct inode *dir)
{
	struct inode *n;
	const git_tree_entry *dentry;
	size_t i, c;
	const char *name;
	mode_t mode;

	if (resolve_tree_obj(repo, dir))
		return 1;

	c = git_tree_entrycount((git_tree *) dir->obj);

	for (i = 0; i < c; i++) {
		dentry = git_tree_entry_byindex((git_tree *) dir->obj, i);
		name = git_tree_entry_name(dentry);
		mode = is_dentry_dir(dentry) ? T_DIR : T_FILE;

		n = add_tree_node(dir, name, T_TREE, mode,
		                  set_oid, (void *) git_tree_entry_id(dentry));
		if (n) {
			n->size = get_node_size(n);
			n->mtime = dir->mtime;
		}
	}

	return 0;
}

static struct inode *
lookup_tree(git_repository *repo, struct inode *dir, const char *entry)
{
	const git_tree_entry *te;
	struct inode *n;
	mode_t mode;

	n = get_tree_child(dir, entry);
	if (!n) {
		if (resolve_tree_obj(repo, dir))
			return NULL;

		te = git_tree_entry_byname((git_tree *)dir->obj, entry);
		if (!te)
			return NULL;

		mode = is_dentry_dir(te) ? T_DIR : T_FILE;
		n = add_tree_node(dir, entry, T_TREE, mode,
		                  set_oid, (void *) git_tree_entry_id(te));
		if (!n)
			return NULL;
	}

	n->size = get_node_size(n);
	n->mtime = dir->mtime;
	return n;
}

static int
open_generic(git_repository *repo, struct inode *file)
{
	int fd = -1;
	int own_obj = 0;
	const char *data = NULL;
	char sha[GIT_HASH_SZ] = {0};
	git_object *obj;

	/* blobs: look up a private copy to avoid racing concurrent opens. */
	if (file->type == T_TREE) {
		if (git_object_lookup(&obj, repo, &file->oid, GIT_OBJECT_ANY))
			return -1;
		own_obj = 1;
	} else {
		obj = aload(&file->obj);
		if (!obj)
			return -1;
	}

	fd = memfd_create(file->name, 0);
	if (fd == -1)
		goto err;

	switch (file->type) {
	case T_TREE:
		data = git_blob_rawcontent((git_blob *)obj);
		break;
	case T_HASH:
		git_oid_tostr(sha, sizeof(sha), git_object_id(obj));
		sha[GIT_HASH_SZ - 1] = '\n';
		data = sha;
		break;
	case T_MSG:
		data = git_commit_message((git_commit *)obj);
		break;
	}

	if (!data)
		goto err;

	file->size = get_node_size(file);

	if (write(fd, data, file->size) != (ssize_t) file->size)
		goto err;

	if (own_obj)
		git_object_free(obj);

	return fd;

err:
	if (fd != -1)
		close(fd);
	if (own_obj)
		git_object_free(obj);
	return -1;
}

static int
__update_head(git_repository *repo, struct inode *n)
{
	git_object *obj, *commit;

	if (git_revparse_single(&obj, repo, "HEAD"))
		return 1;

	if (git_object_peel(&commit, obj, GIT_OBJECT_COMMIT)) {
		git_object_free(obj);
		return 1;
	}

	git_object_free(obj);
	set_commit(n, commit);

	return 0;
}

static int
update_head(git_repository *repo, struct inode *n)
{
	/* Refresh HEAD first */
	if (__update_head(repo, n))
		return 1;
	return update_commit(repo, n);
}

static int
update_objects(git_repository *repo, struct inode *dir)
{
	git_revwalk *walk;
	git_oid oid;
	char hex[GIT_OID_HEXSZ + 1];

	if (git_revwalk_new(&walk, repo))
		return 1;

	if (git_revwalk_push_glob(walk, "refs/*")) {
		git_revwalk_free(walk);
		return 1;
	}

	while (git_revwalk_next(&oid, walk) == 0) {
		git_oid_tostr(hex, sizeof(hex), &oid);
		add_tree_node(dir, hex, T_COMMIT, T_DIR, set_oid, &oid);
	}

	git_revwalk_free(walk);
	return 0;
}

static struct inode *
lookup_objects(git_repository *repo, struct inode *dir, const char *entry)
{
	git_oid oid;
	git_object *obj;
	struct inode *n;

	if (git_oid_fromstr(&oid, entry))
		return NULL;

	if (git_object_lookup(&obj, repo, &oid, GIT_OBJECT_COMMIT))
		return NULL;

	n = add_tree_node(dir, entry, T_COMMIT, T_DIR, set_oid, &oid);
	if (!n) {
		git_object_free(obj);
		return NULL;
	}
	set_commit(n, obj);

	return n;
}

static int
update_generic(git_repository *repo, struct inode *dir)
{
	struct inode *n;

	if (dir->ino == ROOT) {
		n = get_tree_node(HEAD);
		return __update_head(repo, n);
	}

	return 0;
}

static struct inode *
lookup_generic(git_repository *repo, struct inode *dir, const char *entry)
{
	struct inode *n;

	n = get_tree_child(dir, entry);

	if (n && n->ino == HEAD && __update_head(repo, n))
		return NULL;

	return n;
}

struct inode_ops ops[T_ALL] =
{
	{
		.update = update_generic,
		.lookup = lookup_generic,
	},
	{
		.update = update_branches,
		.lookup = lookup_branches,
	},
	{
		.update = update_tags,
		.lookup = lookup_tags,
	},
	{
		.update = update_remotes,
		.lookup = lookup_remotes,
	},
	{
		.update = update_commit,
		.lookup = lookup_commit,
	},
	{
		.update = update_tree,
		.lookup = lookup_tree,
		.open = open_generic,
	},
	{
		.open = open_generic,
	},
	{
		.open = open_generic,
	},
	{
		.update = update_objects,
		.lookup = lookup_objects,
	},
	{
		.update = update_head,
		.lookup = lookup_commit,
	},
};

struct inode_ops *
get_inode_ops(unsigned type)
{
	if (type >= T_ALL)
		return NULL;

	return ops + type;
}
