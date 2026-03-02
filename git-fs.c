#define FUSE_USE_VERSION FUSE_MAKE_VERSION(3, 17)

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fuse3/fuse_lowlevel.h>
#include <git2.h>

#include "inode.h"
#include "tree.h"

#define GITFS_VERSION    "0.1"
#define GIT_CACHE_MAX    (32 << 20)  /* 32 MB */
#define FUSE_CLONE_FD    1
#define GITFS_PERM       0550

/* immutable git objects (trees, blobs) can be cached long */
#define CACHE_IMMUTABLE  86400.0
/* mutable refs (branches, tags, HEAD pointer) need short timeouts */
#define CACHE_REF        1.0

struct gitfs_conf {
	char *mnt;
	char *repo;
	int foreground;
	int passthrough;
	int allow_other;
};

/* per-handle state for open files */
struct gitfs_fh {
	int fd;
	int backing_id;
};

/* per-handle state for open directories */
struct gitfs_dh {
	struct inode *cursor;
	struct inode *head;
};

struct gitfs_tls {
	git_repository *repo;
	git_odb *odb;
};

enum {
	OPT_MOUNT_PATH,
	OPT_REPOSITORY_PATH,
	OPT_FOREGROUND,
	OPT_HELP,
	OPT_VERSION,
	OPT_ALLOW_OTHER,
};

enum {
	DIR_CURRENT = 0,
	DIR_PARENT,
	DIR_DEFAULTS
};

#define GIT_OPT_KEY(t, p, k) { t, offsetof(struct gitfs_conf, p), k }

static struct fuse_opt gitfs_opts[] = {
	GIT_OPT_KEY("-m %s", mnt,		OPT_MOUNT_PATH),
	GIT_OPT_KEY("--mount %s", mnt,		OPT_MOUNT_PATH),
	GIT_OPT_KEY("-r %s", repo, 		OPT_REPOSITORY_PATH),
	GIT_OPT_KEY("--repository %s", repo,	OPT_REPOSITORY_PATH),
	FUSE_OPT_KEY("-f",			OPT_FOREGROUND),
	FUSE_OPT_KEY("--foreground",		OPT_FOREGROUND),
	FUSE_OPT_KEY("-V",			OPT_VERSION),
	FUSE_OPT_KEY("--version",		OPT_VERSION),
	FUSE_OPT_KEY("-a",			OPT_ALLOW_OTHER),
	FUSE_OPT_KEY("--allow-other",		OPT_ALLOW_OTHER),
	FUSE_OPT_KEY("-h",			OPT_HELP),
	FUSE_OPT_KEY("--help",			OPT_HELP),
	FUSE_OPT_END
};

static pthread_key_t gitfs_tls_key;
static struct gitfs_conf conf;

struct gitfs_tls *
get_gitfs_tls(void)
{
	struct gitfs_tls *tls;

	if ((tls = pthread_getspecific(gitfs_tls_key)))
		return tls;

	tls = calloc(1, sizeof(*tls));
	if (tls == NULL)
		return NULL;

	pthread_setspecific(gitfs_tls_key, tls);

	return tls;
}

static git_repository *
get_gitfs_repo(void)
{
	char *repo_path = conf.repo;
	struct gitfs_tls *tls = get_gitfs_tls();

	if (tls == NULL)
		return NULL;
	if (tls->repo)
		return tls->repo;

	if (git_repository_open(&tls->repo, repo_path))
		return NULL;

	return tls->repo;
}

git_odb *
get_gitfs_odb(void)
{
	struct gitfs_tls *tls = get_gitfs_tls();

	if (tls == NULL)
		return NULL;
	if (tls->odb)
		return tls->odb;

	if (!tls->repo && !get_gitfs_repo())
		return NULL;

	if (git_repository_odb(&tls->odb, tls->repo))
		return NULL;

	return tls->odb;
}

static void
thread_cleanup(void *priv)
{
	struct gitfs_tls *tls = priv;

	if (tls == NULL)
		return;
	if (tls->odb)
		git_odb_free(tls->odb);
	if (tls->repo)
		git_repository_free(tls->repo);

	free(tls);
}

static void
gitfs_init(void *priv, struct fuse_conn_info *conn)
{
	git_libgit2_init();
	git_libgit2_opts(GIT_OPT_SET_CACHE_MAX_SIZE, GIT_CACHE_MAX);

	if (tree_init())
		exit(EXIT_FAILURE);

	conn->no_interrupt = 1;
	conf.passthrough = 1;

	if (!fuse_set_feature_flag(conn, FUSE_CAP_PASSTHROUGH)) {
		fprintf(stderr, "git-fs: FUSE passthrough not available, "
		        "falling back to buffered reads\n");
		conf.passthrough = 0;
	}

	pthread_key_create(&gitfs_tls_key, thread_cleanup);
}

static void
gitfs_destroy(void *priv)
{
	tree_destroy();
	git_libgit2_shutdown();
}

static double
cache_timeout(struct inode *n)
{
	switch (n->type) {
	case T_TREE:
	case T_HASH:
	case T_MSG:
	case T_GENERIC:
		return CACHE_IMMUTABLE;
	default:
		return CACHE_REF;
	}
}

static void
fill_stat(struct stat *st, struct inode *n)
{
	st->st_ino = n->ino;
	st->st_uid = getuid();
	st->st_gid = getgid();
	st->st_size = n->size;
	st->st_mode = n->mode | GITFS_PERM;
	st->st_nlink = nlink(n->mode);
	st->st_mtime = n->mtime;
	st->st_atime = n->mtime;
	st->st_ctime = n->mtime;
	st->st_blksize = 4096;
	st->st_blocks = (n->size + 511) / 512;
}

static void
fill_entry(struct fuse_entry_param *e, struct inode *n)
{
	double timeout = cache_timeout(n);

	memset(e, 0, sizeof(*e));
	e->ino = n->ino;
	e->entry_timeout = timeout;
	e->attr_timeout = timeout;
	fill_stat(&e->attr, n);
}

static void
gitfs_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	struct inode *p, *n;
	struct fuse_entry_param e;
	git_repository *repo;

	p = get_tree_node(parent);
	if (!p) {
		fuse_reply_err(req, ENOTDIR);
		return;
	}

	repo = get_gitfs_repo();
	if (!repo) {
		fuse_reply_err(req, EIO);
		return;
	}

	n = p->ops->lookup(repo, p, name);
	if (!n) {
		fuse_reply_err(req, ENOENT);
		return;
	}

	tree_ref(n);
	fill_entry(&e, n);
	fuse_reply_entry(req, &e);
}

static void
gitfs_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	struct stat st;
	struct inode *n;

	n = get_tree_node(ino);
	if (!n) {
		fuse_reply_err(req, ENOENT);
		return;
	}

	memset(&st, 0, sizeof(st));
	fill_stat(&st, n);

	fuse_reply_attr(req, &st, cache_timeout(n));
}

static void
gitfs_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
              off_t off, struct fuse_file_info *fi)
{
	char *buf, *b, *name;
	size_t sz, bsz;
	struct fuse_entry_param e;
	struct inode *n, *p;
	struct gitfs_dh *dh = (struct gitfs_dh *) fi->fh;

	p = get_tree_node(ino);
	if (!p) {
		fuse_reply_err(req, ENOENT);
		return;
	}

	buf = calloc(size, sizeof(char));
	if (!buf) {
		fuse_reply_err(req, ENOMEM);
		return;
	}

	bsz = size;
	for (b = buf; ; b += sz) {
		switch (off) {
		case DIR_CURRENT:
			n = p;
			name = ".";
			break;
		case DIR_PARENT:
			n = p->parent;
			name = "..";
			dh->cursor = dh->head;
			break;
		default:
			n = dh->cursor;
			if (!n)
				goto out;
			/* advance cursor past dead nodes */
			while (aload(&n->flags) & INODE_DELETED) {
				n = aload(&n->sibling);
				if (n == dh->head) {
					n = NULL;
					goto out;
				}
			}
			name = n->name;
		}

		fill_entry(&e, n);
		sz = fuse_add_direntry_plus(req, b, bsz, name, &e, ++off);
		if (sz > bsz)
			break;
		bsz -= sz;

		/* bump nlookup and advance cursor for children */
		if (off > DIR_DEFAULTS) {
			tree_ref(n);
			n = aload(&n->sibling);
			dh->cursor = (n == dh->head) ? NULL : n;
		}
	}

out:
	fuse_reply_buf(req, buf, size - bsz);
	free(buf);
}

static void
gitfs_forget(fuse_req_t req, fuse_ino_t ino, uint64_t nlookup)
{
	struct inode *n;

	n = get_tree_node(ino);
	if (n)
		tree_forget(n, nlookup);

	fuse_reply_none(req);
}

static void
gitfs_forget_multi(fuse_req_t req, size_t count,
                   struct fuse_forget_data *forgets)
{
	size_t i;
	struct inode *n;

	for (i = 0; i < count; i++) {
		n = get_tree_node(forgets[i].ino);
		if (n)
			tree_forget(n, forgets[i].nlookup);
	}

	fuse_reply_none(req);
}

static void
gitfs_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	struct inode *n;
	struct gitfs_dh *dh;
	git_repository *repo;

	n = get_tree_node(ino);
	if (!n) {
		fuse_reply_err(req, ENOENT);
		return;
	}

	repo = get_gitfs_repo();
	if (!repo) {
		fuse_reply_err(req, EIO);
		return;
	}

	dh = calloc(1, sizeof(*dh));
	if (!dh) {
		fuse_reply_err(req, ENOMEM);
		return;
	}

	if (!n->ops->update)
		goto out;

	/* free old children, swap current list out, rebuild from git.
	 * skip T_GENERIC dirs (ROOT, BRANCHES) whose children are static.
	 * skip T_TREE dirs whose git object is immutable by OID. */
	if (n->type != T_GENERIC) {
		if (n->type == T_TREE && aload(&n->child))
			goto out;
		free_retired(axchg(&n->retired, NULL));
		astore(&n->retired, axchg(&n->child, NULL));
	}

	if (n->ops->update(repo, n)) {
		if (n->type != T_GENERIC)
			astore(&n->child, axchg(&n->retired, NULL));
		free(dh);
		fuse_reply_err(req, EIO);
		return;
	}

out:
	dh->head = aload(&n->child);
	fi->fh = (uint64_t) dh;
	fuse_reply_open(req, fi);
}

static void
gitfs_releasedir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	free((struct gitfs_dh *) fi->fh);
	fuse_reply_err(req, 0);
}

static void
gitfs_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	struct inode *n;
	struct gitfs_fh *fh;
	git_repository *repo;
	int id;

	n = get_tree_node(ino);
	if (!n) {
		fuse_reply_err(req, ENOENT);
		return;
	}

	repo = get_gitfs_repo();
	if (!repo) {
		fuse_reply_err(req, EIO);
		return;
	}

	fh = calloc(1, sizeof(*fh));
	if (!fh) {
		fuse_reply_err(req, ENOMEM);
		return;
	}

	fh->fd = n->ops->open(repo, n);
	if (fh->fd == -1) {
		free(fh);
		fuse_reply_err(req, errno);
		return;
	}

	fi->fh = (uint64_t) fh;
	fi->keep_cache = 1;

	if (conf.passthrough) {
		id = fuse_passthrough_open(req, fh->fd);
		if (id == 0) {
			fprintf(stderr, "git-fs: FUSE passthrough not available, "
			        "falling back to buffered reads\n");
			conf.passthrough = 0;
		} else {
			fi->backing_id = fh->backing_id = id;
		}
	}

	fuse_reply_open(req, fi);
}

static void
gitfs_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
           struct fuse_file_info *fi)
{
	struct gitfs_fh *fh = (struct gitfs_fh *) fi->fh;
	struct fuse_bufvec buf = FUSE_BUFVEC_INIT(size);

	buf.buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
	buf.buf[0].fd = fh->fd;
	buf.buf[0].pos = off;

	fuse_reply_data(req, &buf, FUSE_BUF_SPLICE_MOVE);
}

static void
gitfs_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	struct gitfs_fh *fh = (struct gitfs_fh *) fi->fh;

	if (fh->backing_id)
		fuse_passthrough_close(req, fh->backing_id);

	close(fh->fd);
	free(fh);
	fuse_reply_err(req, 0);
}

static const struct fuse_lowlevel_ops ops = {
	.getattr = gitfs_getattr,
	.init = gitfs_init,
	.destroy = gitfs_destroy,
	.lookup = gitfs_lookup,
	.forget = gitfs_forget,
	.forget_multi = gitfs_forget_multi,
	.readdirplus = gitfs_readdir,
	.releasedir = gitfs_releasedir,
	.opendir = gitfs_opendir,
	.open = gitfs_open,
	.read = gitfs_read,
	.release = gitfs_release,
};

void
print_help(void)
{
	fprintf(stderr,
		"usage: git-fs [options]\n"
		"options:\n"
		"\t-m	--mount		File system mount path.\n"
		"\t-r	--repository	Local git repository path.\n"
		"\t-a	--allow-other	Allow other users to access the mount.\n"
		"\t-f	--foreground	Run in the foreground.\n"
		"\t-h	--help		Print this help.\n"
		"\t-V	--version	Print version.\n");
}

static int
gitfs_opt_handler(void *data, const char *arg, int key, struct fuse_args *out)
{
	switch(key) {
	case OPT_ALLOW_OTHER:
		conf.allow_other = 1;
		return 0;
	case OPT_FOREGROUND:
		conf.foreground = 1;
		return 0;
	case OPT_HELP:
		print_help();
		exit(EXIT_SUCCESS);
	case OPT_VERSION:
		fprintf(stderr, "git-fs version %s\n", GITFS_VERSION);
		exit(EXIT_SUCCESS);
	default:
		fprintf(stderr, "Invalid '%s' option.\n", arg);
		print_help();
		exit(EXIT_FAILURE);
	}

	return 1;
}

static void
set_gitfs_conf(struct fuse_args *args, struct gitfs_conf *conf)
{
	char *repo, *mnt;

	if (fuse_opt_parse(args, conf, gitfs_opts, gitfs_opt_handler))
		exit(EXIT_FAILURE);

	if (conf->repo == NULL) {
		fprintf(stderr, "-r/--repository option is mandatory. Aborting.\n");
		exit(EXIT_FAILURE);
	}

	repo = calloc(PATH_MAX, sizeof(char));
	conf->repo = realpath(conf->repo, repo);

	mnt = calloc(PATH_MAX, sizeof(char));
	if (conf->mnt == NULL) {
		snprintf(mnt, PATH_MAX, "%s-fs",  conf->repo);
		conf->mnt = mnt;
	 } else {
		conf->mnt = realpath(conf->mnt, mnt);
	 }
}

static void
set_fuse_args(struct fuse_args *args)
{
	fuse_opt_add_arg(args, "-oauto_unmount,default_permissions,ro");
	if (conf.allow_other)
		fuse_opt_add_arg(args, "-oallow_other");
}

int
main(int argc, char *argv[])
{
	int ret = EXIT_FAILURE;
	struct fuse_session *se;
	struct fuse_loop_config *c;
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

	if (argc == 1) {
		print_help();
		return EXIT_SUCCESS;
	}

	set_gitfs_conf(&args, &conf);
	set_fuse_args(&args);

	se = fuse_session_new(&args, &ops, sizeof(ops), &conf);
	if (se == NULL)
		return ret;

	if (fuse_set_signal_handlers(se) != 0)
		goto err_out1;

	if (fuse_session_mount(se, conf.mnt) != 0)
		goto err_out2;

	fprintf(stderr, "git-fs: mounted at %s "
	        "(unmount with: fusermount3 -u %s)\n",
	        conf.mnt, conf.mnt);

	fuse_daemonize(conf.foreground);

	c = fuse_loop_cfg_create();
	fuse_loop_cfg_set_clone_fd(c, FUSE_CLONE_FD);
	ret = fuse_session_loop_mt(se, c);

	fuse_loop_cfg_destroy(c);
	fuse_session_unmount(se);

err_out2:
	fuse_remove_signal_handlers(se);
err_out1:
	fuse_session_destroy(se);
	return ret;
}
