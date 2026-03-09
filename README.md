# git-fs

Mount a git repository as a read-only FUSE filesystem.

Browse branches, tags, and commits as directories using standard Unix
tools. No checkouts, no working tree copies, no disk overhead per ref.
The filesystem mirrors the repository — pushes, branch updates, and
new tags appear immediately on the next access.

## Usage

```
git-fs -r /path/to/repo [-m /path/to/mountpoint]
```

If `-m` is omitted, the mountpoint defaults to `<repo>-fs` and is
created automatically.

```
$ git-fs -r /srv/linux.git
git-fs: mounted at /srv/linux.git-fs

$ ls /srv/linux.git-fs/
HEAD/  branches/  objects/  tags/

$ cat /srv/linux.git-fs/HEAD/hash
a1b2c3d4e5f6...

$ cat /srv/linux.git-fs/tags/v6.8/tree/Makefile
# SPDX-License-Identifier: GPL-2.0
VERSION = 6
...

$ diff /srv/linux.git-fs/branches/heads/master/tree/init/main.c \
       /srv/linux.git-fs/tags/v6.7/tree/init/main.c
```

To unmount:

```
git-fs -u /srv/linux.git-fs
```

### Options

```
-r, --repository   path to a local git repository (required)
-m, --mount        mountpoint path (default: <repo>-fs)
-u, --unmount      unmount the filesystem at path
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

Requires libfuse3, libgit2, and a C compiler.

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
- Thread safe: Lock-free design with atomic operations.
- Memory-mapped inode pool with batch reclaim via `madvise(2)`.
- File content served through `memfd_create(2)` backed by FUSE passthrough.
- Immutable git objects cached for 24h. Mutable refs refreshed on access.

## License

GPLv3
