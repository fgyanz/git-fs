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
#define FUSE_CLONE_FD    1
#define FUSE_DAEMON      1
#define GITFS_PERM       0550

struct gitfs_conf {
	char *mnt;
	char *repo;
	int passthrough;
};

struct gitfs_tls {
	git_repository *repo;
};

enum {
	OPT_MOUNT_PATH,
	OPT_REPOSITORY_PATH,
	OPT_HELP,
	OPT_VERSION,
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
	FUSE_OPT_KEY("-V",			OPT_VERSION),
	FUSE_OPT_KEY("--version",		OPT_VERSION),
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

git_repository *
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

static void
thread_cleanup(void *priv)
{
	struct gitfs_tls *tls = priv;

	if (tls == NULL)
		return;
	if (tls->repo)
		git_repository_free(tls->repo);

	free(tls);
}

static void
gitfs_init(void *priv, struct fuse_conn_info *conn)
{
	git_libgit2_init();

	if (tree_init())
		exit(EXIT_FAILURE);

	conn->no_interrupt = 1;
	conf.passthrough = 1;

	if (!fuse_set_feature_flag(conn, FUSE_CAP_PASSTHROUGH)) {
		fprintf(stderr, "Failed to set FUSE_CAP_PASSTHROUGH\n");
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

static void
gitfs_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	struct inode *p, *n;
	struct fuse_entry_param e;
	git_repository *repo;

	memset(&e, 0, sizeof(e));

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

	e.ino = n->ino;
	e.attr.st_mode = n->mode | GITFS_PERM;
	e.attr.st_nlink = nlink(n->mode);
	e.attr.st_size = n->size;
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

	st.st_uid = getuid();
	st.st_gid = getgid();
	st.st_size = n->size;
	st.st_mode = n->mode | GITFS_PERM;
	st.st_nlink = nlink(n->mode);

	fuse_reply_attr(req, &st, 1.0);
}

static void
gitfs_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
              off_t off, struct fuse_file_info *fi)
{
	char *buf, *b, *name;
	size_t sz, bsz, ndirs;
	struct stat st;
	struct inode *n, *p;
	off_t i, s;

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

	ndirs = count_tree_children(p) + DIR_DEFAULTS;
	bsz = size;

	for (i = off, b = buf; i < ndirs; b += sz) {
		switch (i) {
		case DIR_PARENT:
			n = p->parent;
			name = "..";
			break;
		case DIR_CURRENT:
			n = p;
			name = ".";
			break;
		default:
			s = i - DIR_DEFAULTS;
			n = s ? get_tree_sibling(aload(&p->child), s)
			      : aload(&p->child);
			if (!n || !n->name) {
				i = ndirs;
				sz = 0;
				continue;
			}
			name = n->name;
		}
		st.st_ino = n->ino;
		st.st_mode = (n->mode | GITFS_PERM) << 12;
		sz = fuse_add_direntry(req, b, bsz, name, &st, ++i);
		if (sz > bsz)
			break;

		bsz -= sz;
	}

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

	if (!n->ops->update)
		goto out;

	/* free old children, swap current list out, rebuild from git.
	 * skip T_GENERIC dirs (ROOT, BRANCHES) whose children are static. */
	if (n->type != T_GENERIC) {
		free_retired(axchg(&n->retired, NULL));
		astore(&n->retired, axchg(&n->child, NULL));
	}

	if (n->ops->update(repo, n)) {
		if (n->type != T_GENERIC)
			astore(&n->child, axchg(&n->retired, NULL));
		fuse_reply_err(req, EIO);
		return;
	}

out:

	fuse_reply_open(req, fi);
}

static void
gitfs_releasedir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	fuse_reply_err(req, 0);
}

static void
gitfs_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	struct inode *n;
	git_repository *repo;
	int fd, id;

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

	fd = n->ops->open(repo, n);
	if (fd == -1) {
		fuse_reply_err(req, errno);
		return;
	}

	fi->fh = fd;
	fi->keep_cache = 0;

	if (conf.passthrough) {
		id = fuse_passthrough_open(req, fd);
		if (id == 0)
			conf.passthrough = 0;
		else
			fi->backing_id = n->backing_id = id;
	}

	fuse_reply_open(req, fi);
}

static void
gitfs_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
           struct fuse_file_info *fi)
{
	fuse_reply_err(req, ENOSYS);
}

static void
gitfs_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	int fd;
	struct inode *n;

	n = get_tree_node(ino);
	if (!n) {
		fuse_reply_err(req, ENOENT);
		return;
	}

	fd = fi->fh;

	if (n->backing_id) {
		fuse_passthrough_close(req, n->backing_id);
		n->backing_id = 0;
	}

	close(fd);
	fuse_reply_err(req, 0);
}

static const struct fuse_lowlevel_ops ops = {
	.getattr = gitfs_getattr,
	.init = gitfs_init,
	.destroy = gitfs_destroy,
	.lookup = gitfs_lookup,
	.forget = gitfs_forget,
	.forget_multi = gitfs_forget_multi,
	.readdir = gitfs_readdir,
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
		"\t-h	--help		Print this help.\n"
		"\t-V	--version	Print version.\n");
}

static int
gitfs_opt_handler(void *data, const char *arg, int key, struct fuse_args *out)
{
	switch(key) {
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

	fuse_daemonize(FUSE_DAEMON);

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
