#ifndef INOTIFY_H
#define INOTIFY_H

struct fuse_session;

extern int  inotify_subscribe(struct fuse_session *se, const char *gitdir);

extern int  inotify_run(void);

extern void inotify_stop(void);

#endif
