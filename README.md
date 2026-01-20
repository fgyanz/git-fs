# git-fs

Mount a git repository as a read-only filesystem.

Browse branches, tags, and commits as directories using standard Unix
tools — no checkouts, no working tree copies, no disk space wasted per
branch. Useful for repositories with many branches (e.g. the Linux
kernel), where scripts need to read files across branches without
cloning or checking out each one.

Unlike git worktrees, git-fs requires no extra disk space per branch.
The entire repository is accessible through a single mountpoint.

## Layout

```
mountpoint/
  HEAD/
    tree/          files at this revision
    msg            commit message
    hash           commit SHA
    parent/        parent commit (same layout, recursive)
  branches/
    heads/         local branches as commit directories
    remotes/
      origin/      remote tracking branches
  tags/            tags as commit directories
```

## Building

Requires libfuse3, libgit2, and pkg-config.

```
make
```

## Usage

```
git-fs -r /path/to/repo -m /path/to/mountpoint
```

The mountpoint directory must exist. To unmount:

```
fusermount3 -u /path/to/mountpoint
```

### Passthrough mode

git-fs uses FUSE passthrough to serve file reads directly from the
kernel, bypassing userspace after open. This requires `CAP_SYS_ADMIN`:

```
make passthrough
```

## Design

- FUSE low-level API for direct request handling
- Per-thread libgit2 handles via thread-local storage
- File content served through memfd backed by FUSE passthrough
- Read-only mount, no working tree modifications
