#!/bin/sh
# Kernel-scale stress test for git-fs.
#
# Sets up a `git clone --shared --no-checkout` of a source repo
# (default: ~/src/linux) into a temp workspace and runs the stress
# test against it with kernel-scale defaults: 64 workers, 200 commits
# sampled at random per round, churn enabled, 2h duration.
#
# The shared clone reuses the source's pack files via objects/info/
# alternates — instant setup, ~MB on disk. Mutations happen only on
# the clone; the source is never modified. The workspace is rm -rf'd
# on exit regardless of how the test finishes.
#
# Environment overrides:
#   SRC             - source repo to clone (default: $HOME/src/linux)
#   GITFS           - path to git-fs binary (default: ./git-fs)
#   DURATION        - wall-clock cap (default: 2h)
#   WORKERS         - parallel readers (default: 64)
#   MAX_COMMITS     - commits sampled per round (default: 200)
#   CHURN_INTERVAL  - sleep between churn ops (default: 0.1)

set -e

SRC="${SRC:-$HOME/src/linux}"
GITFS="${GITFS:-./git-fs}"

if [ ! -x "$GITFS" ]; then
	echo "error: $GITFS not found or not executable" >&2
	exit 1
fi
if [ ! -d "$SRC/.git" ] && [ ! -d "$SRC" ]; then
	echo "error: SRC=$SRC is not a git repository" >&2
	exit 1
fi
if ! git -C "$SRC" rev-parse --git-dir > /dev/null 2>&1; then
	echo "error: SRC=$SRC is not a git repository" >&2
	exit 1
fi

WORKSPACE=$(mktemp -d "${TMPDIR:-/tmp}/git-fs-kernel-stress-XXXXXX")
trap 'rm -rf "$WORKSPACE"' EXIT

echo "test_kernel_stress:"
echo "  source: $SRC"
echo "  workspace: $WORKSPACE"
echo "  cloning (shared, no-checkout)..."

# --shared makes the clone use the source's object store via alternates
# (no copy). --no-checkout skips populating the working tree.
git clone --shared --no-checkout --quiet "$SRC" "$WORKSPACE/repo"

# count what we got
n_commits=$(git -C "$WORKSPACE/repo" rev-list --all 2>/dev/null | wc -l)
n_tags=$(git -C "$WORKSPACE/repo" tag -l 2>/dev/null | wc -l)
n_branches=$(git -C "$WORKSPACE/repo" for-each-ref --format='%(refname)' \
	refs/heads/ refs/remotes/ 2>/dev/null | wc -l)
echo "  clone: $n_commits commits, $n_tags tags, $n_branches branches/remotes"

# note: do NOT exec — the EXIT trap on this shell is what cleans up
# $WORKSPACE. exec would replace this shell with test_stress.sh and
# the trap would never fire.
REPO="$WORKSPACE/repo" \
DURATION="${DURATION:-2h}" \
WORKERS="${WORKERS:-64}" \
MAX_COMMITS="${MAX_COMMITS:-200}" \
CHURN=1 \
CHURN_INTERVAL="${CHURN_INTERVAL:-0.1}" \
RANDOM_SAMPLE=1 \
ROUNDS="${ROUNDS:-100000}" \
	./tests/test_stress.sh "$GITFS"
