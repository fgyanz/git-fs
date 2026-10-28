#!/bin/sh
# Kernel-scale stress test for git-fs: runs test_stress.sh with churn
# on a shared clone of SRC, so the source repo is never modified.
#
# Environment overrides:
#   SRC             - source repo to clone (default: $HOME/src/linux)
#   GITFS           - path to git-fs binary (default: ./git-fs)
#   DURATION        - wall-clock cap (default: 2h)
#   WORKERS         - parallel readers (default: 64)
#   MAX_COMMITS     - commits sampled per round (default: 200)
#   CHURN_INTERVAL  - sleep between churn ops (default: 0.1)

set -e

. "$(dirname "$0")/lib.sh"
use_gitfs "${GITFS:-./git-fs}"

SRC="${SRC:-$HOME/src/linux}"

if [ ! -d "$SRC/.git" ] && [ ! -d "$SRC" ]; then
	echo "error: SRC=$SRC is not a git repository" >&2
	exit 1
fi
if ! git -C "$SRC" rev-parse --git-dir > /dev/null 2>&1; then
	echo "error: SRC=$SRC is not a git repository" >&2
	exit 1
fi

WORKSPACE="$TESTDIR"

echo "test_kernel_stress:"
echo "  source: $SRC"
echo "  workspace: $WORKSPACE"
echo "  cloning (shared, no-checkout)..."

git clone --shared --no-checkout --quiet "$SRC" "$WORKSPACE/repo"

n_commits=$(git -C "$WORKSPACE/repo" rev-list --all 2>/dev/null | wc -l)
n_tags=$(git -C "$WORKSPACE/repo" tag -l 2>/dev/null | wc -l)
n_branches=$(git -C "$WORKSPACE/repo" for-each-ref --format='%(refname)' \
	refs/heads/ refs/remotes/ 2>/dev/null | wc -l)
echo "  clone: $n_commits commits, $n_tags tags, $n_branches branches/remotes"

# no exec: lib.sh's EXIT trap removes the clone
REPO="$WORKSPACE/repo" \
DURATION="${DURATION:-2h}" \
WORKERS="${WORKERS:-64}" \
MAX_COMMITS="${MAX_COMMITS:-200}" \
CHURN=1 \
CHURN_INTERVAL="${CHURN_INTERVAL:-0.1}" \
RANDOM_SAMPLE=1 \
ROUNDS="${ROUNDS:-100000}" \
	./tests/test_stress.sh "$GITFS"
