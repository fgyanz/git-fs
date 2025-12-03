#define FUSE_USE_VERSION 312

#include <fuse3/fuse_lowlevel.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>

#define GITFS_VERSION 	"0.1"
#define FUSE_CLONE_FD	1
#define FUSE_DAEMON	1

struct gitfs_conf {
	char *mnt;
	char *repo;
};

enum {
	OPT_MOUNT_PATH,
	OPT_REPOSITORY_PATH,
	OPT_HELP,
	OPT_VERSION,
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

static void
gitfs_init(void *userdata, struct fuse_conn_info *conn)
{
	return;
}

static void
gitfs_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	return;
}

static const struct fuse_lowlevel_ops ops = {
	.getattr = gitfs_getattr,
	.init = gitfs_init,
	/*
	 * .open
	 * .read
	 * .opendir
	 * .readdir
	 * .lookup
	 */
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

	memset(conf, 0, sizeof(*conf));

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
	//fuse_opt_add_arg(args, "auto_cache");
}

int
main(int argc, char *argv[])
{
	int ret = EXIT_FAILURE;
	struct fuse_session *se;
	struct fuse_loop_config *c;
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct gitfs_conf conf;

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
