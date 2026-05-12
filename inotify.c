/* TODO: Careful champ.
 * This file was mostly vibe-coded to test the potential of an AI.
 * A more in-depth review is required.
 */

#define FUSE_USE_VERSION FUSE_MAKE_VERSION(3, 17)

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <fuse3/fuse_lowlevel.h>
#include <git2.h>

#include "inode.h"
#include "inotify.h"
#include "tree.h"

#define DIRTY_HEAD    (1u << 0)
#define DIRTY_HEADS   (1u << 1)
#define DIRTY_TAGS    (1u << 2)
#define DIRTY_REMOTES (1u << 3)
#define DIRTY_ALL     (DIRTY_HEAD | DIRTY_HEADS | DIRTY_TAGS | DIRTY_REMOTES)

static struct {
	int ino_fd;
	int stop_fd;
	int head_wd;
	int heads_wd;
	int tags_wd;
	int remotes_wd;
	pthread_t tid;
	struct fuse_session *session;
	char gitdir[PATH_MAX];
} w;

/* invalidate kernel dentry cache for every name in the retired ring. */
static void
inval_children(struct inode *p, struct inode *head)
{
	struct inode *c;

	if (!head)
		return;
	c = head;
	do {
		fuse_lowlevel_notify_inval_entry(w.session, p->ino,
			c->name, strlen(c->name));
		c = aload(&c->sibling);
	} while (c != head);
}

/* retire p's child ring and rebuild it. */
static void
refresh_listing(git_repository *repo, struct inode *p)
{
	struct inode *retired;

	if (inode_acquire(p))
		return;

	afadd(&p->rebuild_seq, 1);

	free_retired(axchg(&p->retired, NULL));
	retired = axchg(&p->child, NULL);
	astore(&p->retired, retired);

	p->ops->update(repo, p);

	afor(&p->flags, INODE_READY);
	afand(&p->flags, ~INODE_POPULATING);

	inval_children(p, retired);
	fuse_lowlevel_notify_inval_inode(w.session, p->ino, 0, 0);
}

/* map an event to the listings it dirtiesg. */
static unsigned
classify_event(struct inotify_event *ev)
{
	if (ev->mask & IN_Q_OVERFLOW)
		return DIRTY_ALL;

	if (ev->wd == w.head_wd && ev->len > 0) {
		if (!strcmp(ev->name, "HEAD"))
			return DIRTY_HEAD;
		if (!strcmp(ev->name, "packed-refs"))
			return DIRTY_ALL;
		return 0;
	}

	if (ev->wd == w.heads_wd)
		return DIRTY_HEAD | DIRTY_HEADS;
	if (ev->wd == w.tags_wd)
		return DIRTY_TAGS;
	if (ev->wd == w.remotes_wd)
		return DIRTY_REMOTES;

	return 0;
}

static int
drain_events(unsigned *dirty)
{
	union {
		/* Force alignment on buf */
		struct inotify_event ev;
		char buf[4096];
	} u;
	struct inotify_event *ev;
	ssize_t n;
	char *p;

	for (;;) {
		n = read(w.ino_fd, u.buf, sizeof(u.buf));
		if (n < 0) {
			if (errno == EAGAIN)
				return 0;
			if (errno == EINTR)
				continue;
			return -1;
		}
		for (p = u.buf; p < u.buf + n; p += sizeof(*ev) + ev->len) {
			ev = (struct inotify_event *) p;
			*dirty |= classify_event(ev);
		}
	}
}

static void
dispatch(git_repository *repo, unsigned dirty)
{
	if (dirty & DIRTY_HEAD)
		refresh_listing(repo, get_tree_node(HEAD));
	if (dirty & DIRTY_HEADS)
		refresh_listing(repo, get_tree_node(HEADS));
	if (dirty & DIRTY_TAGS)
		refresh_listing(repo, get_tree_node(TAGS));
	if (dirty & DIRTY_REMOTES)
		refresh_listing(repo, get_tree_node(REMOTES));
}

static void *
watcher_loop(void *priv)
{
	struct pollfd fds[2];
	git_repository *repo;
	unsigned dirty;

	(void) priv;

	/* libgit2 handles aren't safe to share across threads */
	if (git_repository_open(&repo, w.gitdir)) {
		fprintf(stderr, "git-fs: watcher: failed to open repo at %s\n",
		        w.gitdir);
		return NULL;
	}

	fds[0].fd = w.ino_fd;
	fds[0].events = POLLIN;
	fds[1].fd = w.stop_fd;
	fds[1].events = POLLIN;

	for (;;) {
		if (poll(fds, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (fds[1].revents & POLLIN)
			break;
		if (!(fds[0].revents & POLLIN))
			continue;

		dirty = 0;
		if (drain_events(&dirty))
			break;
		dispatch(repo, dirty);
	}

	git_repository_free(repo);
	return NULL;
}

int
inotify_subscribe(struct fuse_session *se, const char *gitdir)
{
	char p[PATH_MAX];
	uint32_t mask;

	w.ino_fd = -1;
	w.stop_fd = -1;
	w.head_wd = -1;
	w.heads_wd = -1;
	w.tags_wd = -1;
	w.remotes_wd = -1;
	w.session = se;

	snprintf(w.gitdir, sizeof(w.gitdir), "%s", gitdir);

	w.ino_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (w.ino_fd < 0)
		return 1;

	w.stop_fd = eventfd(0, EFD_CLOEXEC);
	if (w.stop_fd < 0)
		goto err;

	/* watch the .git/ root for HEAD and packed-refs writes. */
	w.head_wd = inotify_add_watch(w.ino_fd, gitdir,
	                              IN_CLOSE_WRITE | IN_MOVED_TO);
	if (w.head_wd < 0)
		goto err;

	mask = IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE |
	       IN_DELETE | IN_MOVED_FROM;

	snprintf(p, sizeof(p), "%srefs/heads", gitdir);
	w.heads_wd = inotify_add_watch(w.ino_fd, p, mask);
	if (w.heads_wd < 0)
		goto err;

	snprintf(p, sizeof(p), "%srefs/tags", gitdir);
	w.tags_wd = inotify_add_watch(w.ino_fd, p, mask);
	if (w.tags_wd < 0)
		goto err;

	/* refs/remotes is optional: clones with no remotes don't have it. */
	snprintf(p, sizeof(p), "%srefs/remotes", gitdir);
	w.remotes_wd = inotify_add_watch(w.ino_fd, p, mask);

	return 0;

err:
	if (w.stop_fd >= 0)
		close(w.stop_fd);
	if (w.ino_fd >= 0)
		close(w.ino_fd);
	w.stop_fd = -1;
	w.ino_fd = -1;
	return 1;
}

int
inotify_run(void)
{
	if (pthread_create(&w.tid, NULL, watcher_loop, NULL)) {
		close(w.ino_fd);
		close(w.stop_fd);
		w.ino_fd = -1;
		w.stop_fd = -1;
		return 1;
	}
	return 0;
}

void
inotify_stop(void)
{
	if (w.stop_fd < 0)
		return;

	if (eventfd_write(w.stop_fd, 1) < 0)
		fprintf(stderr, "git-fs: inotify_stop: stop write failed: %s\n",
		        strerror(errno));
	else
		pthread_join(w.tid, NULL);

	close(w.ino_fd);
	close(w.stop_fd);
	w.ino_fd = -1;
	w.stop_fd = -1;
}
