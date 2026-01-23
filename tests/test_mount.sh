#!/bin/sh
# Integration tests for git-fs
# Usage: ./tests/test_mount.sh [/path/to/git-fs-binary]
#
# Creates a temporary git repo and mount point, mounts it,
# and verifies the filesystem layout.

set -e

GITFS="${1:-./git-fs}"
PASS=0
FAIL=0

if [ ! -x "$GITFS" ]; then
	echo "error: $GITFS not found or not executable" >&2
	exit 1
fi

cleanup() {
	fusermount3 -u "$MNT" 2>/dev/null || true
	rm -rf "$REPO" "$MNT"
}

assert_eq() {
	if [ "$1" = "$2" ]; then
		PASS=$((PASS + 1))
		printf "  %-40s  ok\n" "$3"
	else
		FAIL=$((FAIL + 1))
		printf "  FAIL %-34s  got '%s', expected '%s'\n" "$3" "$1" "$2"
	fi
}

assert_contains() {
	if echo "$1" | grep -q "$2"; then
		PASS=$((PASS + 1))
		printf "  %-40s  ok\n" "$3"
	else
		FAIL=$((FAIL + 1))
		printf "  FAIL %-34s  '%s' not found in output\n" "$3" "$2"
	fi
}

assert_match() {
	if echo "$1" | grep -qE "$2"; then
		PASS=$((PASS + 1))
		printf "  %-40s  ok\n" "$3"
	else
		FAIL=$((FAIL + 1))
		printf "  FAIL %-34s  pattern '%s' not matched\n" "$3" "$2"
	fi
}

# Create test repo
REPO=$(mktemp -d /tmp/git-fs-test-repo-XXXXXX)
MNT=$(mktemp -d /tmp/git-fs-test-mnt-XXXXXX)
trap cleanup EXIT

cd "$REPO"
git init -q
git config user.email "test@test"
git config user.name "test"
echo "hello" > file.txt
mkdir subdir
echo "nested" > subdir/inner.txt
git add .
git commit -q -m "first commit"
echo "world" > file2.txt
git add file2.txt
git commit -q -m "second commit"
git tag v1.0
git branch feature
cd - > /dev/null

echo "test_mount:"

# Mount
"$GITFS" -r "$REPO" -m "$MNT"
sleep 0.5

# Test: root directory listing
ROOT_LS=$(ls "$MNT")
assert_contains "$ROOT_LS" "HEAD" "root_has_HEAD"
assert_contains "$ROOT_LS" "branches" "root_has_branches"
assert_contains "$ROOT_LS" "tags" "root_has_tags"

# Test: HEAD hash is valid 40-char hex
HASH=$(cat "$MNT/HEAD/hash")
assert_match "$HASH" "^[0-9a-f]{40}$" "HEAD_hash_valid"

# Test: HEAD msg contains commit message
MSG=$(cat "$MNT/HEAD/msg")
assert_contains "$MSG" "second commit" "HEAD_msg_content"

# Test: HEAD tree lists files
TREE_LS=$(ls "$MNT/HEAD/tree/")
assert_contains "$TREE_LS" "file.txt" "tree_has_file"
assert_contains "$TREE_LS" "file2.txt" "tree_has_file2"
assert_contains "$TREE_LS" "subdir" "tree_has_subdir"

# Test: file content
FILE_CONTENT=$(cat "$MNT/HEAD/tree/file.txt")
assert_eq "$FILE_CONTENT" "hello" "file_content"

# Test: nested file
NESTED=$(cat "$MNT/HEAD/tree/subdir/inner.txt")
assert_eq "$NESTED" "nested" "nested_file_content"

# Test: branches
BR_LS=$(ls "$MNT/branches/heads/")
assert_contains "$BR_LS" "feature" "branches_has_feature"

# Test: tags
TAG_LS=$(ls "$MNT/tags/")
assert_contains "$TAG_LS" "v1.0" "tags_has_v1.0"

# Test: tag commit
TAG_MSG=$(cat "$MNT/tags/v1.0/msg")
assert_contains "$TAG_MSG" "second commit" "tag_msg_content"

# Test: parent commit
PARENT_MSG=$(cat "$MNT/HEAD/parent/msg")
assert_contains "$PARENT_MSG" "first commit" "parent_msg_content"

# Unmount
fusermount3 -u "$MNT"
assert_eq "$?" "0" "unmount_succeeds"

echo
TOTAL=$((PASS + FAIL))
echo "$PASS/$TOTAL tests passed"
[ "$FAIL" -eq 0 ]
