#!/bin/sh
# Run the test suites against a sanitizer build.
# Usage: ./tests/test_sanitize.sh asan|tsan [/path/to/git-fs-MODE]

set -e

. "$(dirname "$0")/lib.sh"
MODE="$1"
LOGDIR="$TESTDIR"

case "$MODE" in
asan)
	# libgit2 has no frame pointers: the slow unwinder reaches git-fs code
	export ASAN_OPTIONS="detect_leaks=1:fast_unwind_on_malloc=0"
	export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"
	;;
tsan)
	export TSAN_OPTIONS="second_deadlock_stack=1"
	;;
*)
	echo "usage: $0 asan|tsan [/path/to/git-fs-MODE]" >&2
	exit 1
	;;
esac
use_gitfs "${2:-./git-fs-$MODE}"

status=0
"./tests/test_tree-$MODE" || status=1
"./tests/test_inode-$MODE" || status=1

# git-fs loses stderr when it daemonizes
ASAN_OPTIONS="$ASAN_OPTIONS:log_path=$LOGDIR/asan"
UBSAN_OPTIONS="$UBSAN_OPTIONS:log_path=$LOGDIR/ubsan"
TSAN_OPTIONS="$TSAN_OPTIONS:log_path=$LOGDIR/tsan"

./tests/test_mount.sh "$GITFS" || status=1
# shadow memory and quarantine distort fd limits and RSS
SKIP="R5" MEM_CHECKS=0 ./tests/test_resources.sh "$GITFS" || status=1

if grep -q -E 'ERROR: (Address|Leak)Sanitizer|runtime error|WARNING: ThreadSanitizer' \
	"$LOGDIR"/* 2>/dev/null; then
	echo
	echo "=== sanitizer reports ==="
	cat "$LOGDIR"/*
	status=1
fi
exit $status
