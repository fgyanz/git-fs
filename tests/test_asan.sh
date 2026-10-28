#!/bin/sh
# Run the test suites against the sanitizer builds (make test-asan).
# Usage: ./tests/test_asan.sh [/path/to/git-fs-asan]

set -e

. "$(dirname "$0")/lib.sh"
use_gitfs "${1:-./git-fs-asan}"
LOGDIR="$TESTDIR"

# libgit2 has no frame pointers: the slow unwinder reaches git-fs code
export ASAN_OPTIONS="detect_leaks=1:fast_unwind_on_malloc=0"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"

status=0
./tests/test_tree-asan || status=1
./tests/test_inode-asan || status=1

# git-fs loses stderr when it daemonizes
ASAN_OPTIONS="$ASAN_OPTIONS:log_path=$LOGDIR/asan"
UBSAN_OPTIONS="$UBSAN_OPTIONS:log_path=$LOGDIR/ubsan"

./tests/test_mount.sh "$GITFS" || status=1
# ASan's shadow memory and quarantine distort fd limits and RSS
SKIP="R5" MEM_CHECKS=0 ./tests/test_resources.sh "$GITFS" || status=1

if grep -q -E 'ERROR: (AddressSanitizer|LeakSanitizer)|runtime error' \
	"$LOGDIR"/* 2>/dev/null; then
	echo
	echo "=== sanitizer reports ==="
	cat "$LOGDIR"/*
	status=1
fi
exit $status
