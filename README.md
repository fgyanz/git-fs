# git-fs

Mount a git repository as a read-only FUSE filesystem on Linux.

Access every branch, tag, and commit simultaneously as directories
using standard Unix tools. No checkouts, no working tree copies, no
disk overhead per ref. The filesystem mirrors the repository: pushes,
branch updates, and new tags appear immediately on the next access.

Useful for:

- **Analysis**: feed multiple revisions into any tool that reads
  files: linters, code review, license scanners, search indexers.
- **Scripting**: read files across refs with plain paths instead of
  `git show` and `git diff` plumbing.
- **Browsing**: explore history with `ls`, `cat`, and `diff`.

On long-running server mounts with warm caches, multi-ref reads can
outperform equivalent git commands. Shared content across refs is
served from cache, not re-read from disk.

## Usage

Must be run from inside a git repository. git-fs discovers the repo from
the current directory and by default mounts at `.git/fs`/.

```
$ cd /path/to/repo
$ git-fs
$ ls .git/fs/
HEAD/  branches/  objects/  tags/

$ cat .git/fs/HEAD/hash
a1b2c3d4e5f6...

$ cat .git/fs/tags/v6.8/tree/Makefile
# SPDX-License-Identifier: GPL-2.0
VERSION = 6
...

# diff across refs — no checkouts, no intermediate files
$ diff .git/fs/branches/heads/master/tree/init/main.c \
       .git/fs/tags/v6.7/tree/init/main.c

# grep across all maintenance branches at once
$ grep -rl "CVE-2024" .git/fs/branches/heads/linux-6.*/tree/

# feed multiple revisions into an analysis tool
$ for tag in .git/fs/tags/v6.{5,6,7,8}; do
    analyze "$tag/tree/kernel/"
  done
```

Custom mountpoint:

```
$ cd /srv/linux.git
$ git-fs -m /mnt/linux
```

To unmount, run from inside the repo (defaults to `.git/fs`) or pass
an explicit path:

```
git-fs -u                    # unmount <gitdir>/fs of the cwd repo
git-fs -u /mnt/linux         # unmount a custom mountpoint
```

### Options

```
-r, --repository   path to a local git repository (required)
-m, --mount        mountpoint path
-u, --unmount      unmount path
-a, --allow-other  allow other users to access the mount
-f, --foreground   run in the foreground
-V, --version      print version
-h, --help         print help
```

## Layout

Each ref (branch, tag, or commit SHA) is a directory with the same
structure. File sizes and mtimes reflect the actual commit — `ls -l`
and `stat(1)` work as expected:

```
<ref>/
  tree/     files at this revision
  hash      commit SHA
  msg       commit message
  parent/   parent commit (same layout, recursive)
```

The full mountpoint:

```
mountpoint/
  HEAD/                   current HEAD commit
  branches/
    heads/                local branches
    remotes/
      <remote>/           remote tracking branches
  tags/                   tags
  objects/                all commits by SHA
```

## Building

Linux only. Requires libfuse3, libgit2, and a C compiler (GCC or
Clang). Tested with glibc and musl.

```
make
make install          # installs to /usr/local/bin
```

### FUSE passthrough

When available, git-fs uses FUSE passthrough to serve file reads
directly from the kernel page cache, bypassing userspace after open.
This requires `CAP_SYS_ADMIN`:

```
make passthrough      # install + setcap cap_sys_admin+ep
```

Falls back to buffered reads automatically if unavailable.

### Tests

```
make test
```

## Design

- FUSE low-level API — no libfuse high-level overhead.
- Read-only. No write path in the code — the kernel enforces `ro` mount.
- Thread safe: lock-free design with atomic operations.
- Memory-mapped inode pool with batch reclaim via `madvise(2)`.
- File content served through `memfd_create(2)` backed by FUSE passthrough.
- Immutable git objects cached for 24h. Mutable refs refreshed on access.
- Content-addressed caching — identical blobs and trees across refs are
  resolved once, making multi-ref operations faster than repeated git
  commands.

## Inspired by

Plan 9's [git/fs](https://orib.dev/git9.html) from git9.

## License

GPLv3
