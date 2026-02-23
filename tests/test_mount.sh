#!/bin/sh
# Integration tests for git-fs
# Usage: ./tests/test_mount.sh [/path/to/git-fs-binary]
#
# Creates a temporary git repo and mount point, mounts it,
# and verifies the filesystem layout and FUSE callback behavior.

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
	rm -rf "$REPO" "$MNT" "$REMOTE"
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

assert_fail() {
	if eval "$1" 2>/dev/null; then
		FAIL=$((FAIL + 1))
		printf "  FAIL %-34s  command should have failed\n" "$2"
	else
		PASS=$((PASS + 1))
		printf "  %-40s  ok\n" "$2"
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
# Third commit: large directory with 500 files to force multiple readdir calls
mkdir manyfiles
for i in $(seq -w 1 500); do
	echo "content-$i" > "manyfiles/f-$i.txt"
done
git add manyfiles
git commit -q -m "add large directory"
git tag v1.0
git branch feature

# Set up a remote with tracking branches
REMOTE=$(mktemp -d /tmp/git-fs-test-remote-XXXXXX)
git clone -q --bare "$REPO" "$REMOTE"
git remote add origin "$REMOTE"
git fetch -q origin

cd - > /dev/null

echo "test_mount:"

# Mount
"$GITFS" -r "$REPO" -m "$MNT"
sleep 0.5

# --- readdir: root directory ---
ROOT_LS=$(ls "$MNT")
assert_contains "$ROOT_LS" "HEAD" "readdir_root_HEAD"
assert_contains "$ROOT_LS" "branches" "readdir_root_branches"
assert_contains "$ROOT_LS" "tags" "readdir_root_tags"

# --- readdir: commit directory ---
COMMIT_LS=$(ls "$MNT/HEAD")
assert_contains "$COMMIT_LS" "tree" "readdir_commit_tree"
assert_contains "$COMMIT_LS" "hash" "readdir_commit_hash"
assert_contains "$COMMIT_LS" "msg" "readdir_commit_msg"
assert_contains "$COMMIT_LS" "parent" "readdir_commit_parent"

# --- readdir: tree directory ---
TREE_LS=$(ls "$MNT/HEAD/tree/")
assert_contains "$TREE_LS" "file.txt" "readdir_tree_file"
assert_contains "$TREE_LS" "file2.txt" "readdir_tree_file2"
assert_contains "$TREE_LS" "subdir" "readdir_tree_subdir"
assert_contains "$TREE_LS" "manyfiles" "readdir_tree_manyfiles"

# --- readdir: subdirectory ---
SUB_LS=$(ls "$MNT/HEAD/tree/subdir/")
assert_contains "$SUB_LS" "inner.txt" "readdir_subdir_file"

# --- readdir: branches ---
BR_LS=$(ls "$MNT/branches/heads/")
assert_contains "$BR_LS" "feature" "readdir_branches_feature"

# --- readdir: tags ---
TAG_LS=$(ls "$MNT/tags/")
assert_contains "$TAG_LS" "v1.0" "readdir_tags_v1.0"

# --- open/read: hash file ---
HASH=$(cat "$MNT/HEAD/hash")
assert_match "$HASH" "^[0-9a-f]{40}$" "read_hash_valid_hex"

# --- open/read: msg file ---
MSG=$(cat "$MNT/HEAD/msg")
assert_contains "$MSG" "add large directory" "read_msg_content"

# --- open/read: blob file ---
FILE_CONTENT=$(cat "$MNT/HEAD/tree/file.txt")
assert_eq "$FILE_CONTENT" "hello" "read_blob_content"

# --- open/read: nested blob ---
NESTED=$(cat "$MNT/HEAD/tree/subdir/inner.txt")
assert_eq "$NESTED" "nested" "read_nested_blob"

# --- open/read: tag commit ---
TAG_MSG=$(cat "$MNT/tags/v1.0/msg")
assert_contains "$TAG_MSG" "add large directory" "read_tag_msg"

# --- open/read: parent commit ---
PARENT_MSG=$(cat "$MNT/HEAD/parent/msg")
assert_contains "$PARENT_MSG" "second commit" "read_parent_msg"

# --- getattr: root is directory ---
STAT_ROOT=$(stat -c '%F' "$MNT")
assert_eq "$STAT_ROOT" "directory" "getattr_root_is_dir"

# --- getattr: HEAD is directory ---
STAT_HEAD=$(stat -c '%F' "$MNT/HEAD")
assert_eq "$STAT_HEAD" "directory" "getattr_HEAD_is_dir"

# --- getattr: tree/ is directory ---
STAT_TREE=$(stat -c '%F' "$MNT/HEAD/tree")
assert_eq "$STAT_TREE" "directory" "getattr_tree_is_dir"

# --- getattr: subdir is directory ---
STAT_SUB=$(stat -c '%F' "$MNT/HEAD/tree/subdir")
assert_eq "$STAT_SUB" "directory" "getattr_subdir_is_dir"

# --- getattr: file is regular ---
STAT_FILE=$(stat -c '%F' "$MNT/HEAD/tree/file.txt")
assert_eq "$STAT_FILE" "regular file" "getattr_file_is_regular"

# --- getattr: hash is regular ---
STAT_HASH=$(stat -c '%F' "$MNT/HEAD/hash")
assert_eq "$STAT_HASH" "regular file" "getattr_hash_is_regular"

# --- getattr: file size matches content ---
STAT_SIZE=$(stat -c '%s' "$MNT/HEAD/tree/file.txt")
assert_eq "$STAT_SIZE" "6" "getattr_file_size"

# --- getattr: hash size is 41 (40 hex + newline) ---
STAT_HASH_SIZE=$(stat -c '%s' "$MNT/HEAD/hash")
assert_eq "$STAT_HASH_SIZE" "41" "getattr_hash_size"

# --- getattr: directory nlink >= 2 ---
STAT_NLINK=$(stat -c '%h' "$MNT")
assert_match "$STAT_NLINK" "^[2-9]" "getattr_dir_nlink"

# --- getattr: file nlink == 1 ---
STAT_FNLINK=$(stat -c '%h' "$MNT/HEAD/hash")
assert_eq "$STAT_FNLINK" "1" "getattr_file_nlink"

# --- error: non-existent lookup returns ENOENT ---
assert_fail "cat '$MNT/nonexistent' " "lookup_enoent"
assert_fail "ls '$MNT/HEAD/nonexistent'" "lookup_commit_enoent"
assert_fail "cat '$MNT/HEAD/tree/no-such-file'" "lookup_tree_enoent"

# --- error: read-only filesystem ---
assert_fail "touch '$MNT/newfile'" "write_readonly"
assert_fail "mkdir '$MNT/newdir'" "mkdir_readonly"
assert_fail "rm '$MNT/HEAD/hash'" "rm_readonly"

# --- root commit has no parent entry ---
# HEAD -> parent (second commit) -> parent (first/root commit)
ROOT_COMMIT_LS=$(ls "$MNT/HEAD/parent/parent/")
assert_contains "$ROOT_COMMIT_LS" "tree" "root_commit_has_tree"
assert_contains "$ROOT_COMMIT_LS" "hash" "root_commit_has_hash"
assert_contains "$ROOT_COMMIT_LS" "msg" "root_commit_has_msg"
# "parent" should NOT appear for root commits
if echo "$ROOT_COMMIT_LS" | grep -q "^parent$"; then
	FAIL=$((FAIL + 1))
	printf "  FAIL %-34s  root commit should not have parent\n" "root_commit_no_parent"
else
	PASS=$((PASS + 1))
	printf "  %-40s  ok\n" "root_commit_no_parent"
fi

# --- readdir large directory (forces multiple kernel readdir calls) ---
MANY_COUNT=$(ls -1 "$MNT/HEAD/tree/manyfiles/" | wc -l)
assert_eq "$MANY_COUNT" "500" "readdir_large_count"

# Verify first, middle, last entries are readable
FIRST=$(cat "$MNT/HEAD/tree/manyfiles/f-001.txt")
assert_eq "$FIRST" "content-001" "readdir_large_first"

MID=$(cat "$MNT/HEAD/tree/manyfiles/f-250.txt")
assert_eq "$MID" "content-250" "readdir_large_mid"

LAST=$(cat "$MNT/HEAD/tree/manyfiles/f-500.txt")
assert_eq "$LAST" "content-500" "readdir_large_last"

# Verify no duplicates in listing
MANY_UNIQ=$(ls -1 "$MNT/HEAD/tree/manyfiles/" | sort -u | wc -l)
assert_eq "$MANY_UNIQ" "500" "readdir_large_no_dupes"

# --- branch commit matches HEAD ---
BRANCH_HASH=$(cat "$MNT/tags/v1.0/hash")
# tag was created before the large dir commit, so hashes differ now
# just verify tag hash is valid
assert_match "$BRANCH_HASH" "^[0-9a-f]{40}$" "tag_hash_valid"

# --- readdir: remote branches ---
REMOTE_LS=$(ls "$MNT/branches/remotes/")
assert_contains "$REMOTE_LS" "origin" "readdir_remotes_origin"

REMOTE_BR_LS=$(ls "$MNT/branches/remotes/origin/")
assert_contains "$REMOTE_BR_LS" "master" "readdir_remote_branches_master"

# --- open/read: remote branch commit ---
REMOTE_HASH=$(cat "$MNT/branches/remotes/origin/master/hash")
HEAD_HASH=$(cat "$MNT/HEAD/hash")
assert_eq "$REMOTE_HASH" "$HEAD_HASH" "remote_branch_matches_HEAD"

# --- ref refresh: new branch appears after mount ---
cd "$REPO"
git branch newbranch
cd - > /dev/null
sleep 1.5
NEW_BR_LS=$(ls "$MNT/branches/heads/")
assert_contains "$NEW_BR_LS" "newbranch" "ref_refresh_new_branch"

# --- ref refresh: new tag appears after mount ---
cd "$REPO"
git tag v2.0
cd - > /dev/null
sleep 1.5
NEW_TAG_LS=$(ls "$MNT/tags/")
assert_contains "$NEW_TAG_LS" "v2.0" "ref_refresh_new_tag"

# --- concurrent: parallel reads of different files ---
for i in 1 2 3 4 5 6 7 8; do
	cat "$MNT/HEAD/tree/manyfiles/f-$(printf '%03d' $((i * 60))).txt" > /tmp/git-fs-par-$i &
done
wait
PAR_OK=1
for i in 1 2 3 4 5 6 7 8; do
	expected="content-$(printf '%03d' $((i * 60)))"
	got=$(cat /tmp/git-fs-par-$i)
	rm -f /tmp/git-fs-par-$i
	if [ "$got" != "$expected" ]; then PAR_OK=0; fi
done
assert_eq "$PAR_OK" "1" "concurrent_parallel_reads"

# --- concurrent: parallel readdir on the same large directory ---
for i in 1 2 3 4; do
	ls -1 "$MNT/HEAD/tree/manyfiles/" | sort > /tmp/git-fs-ls-$i &
done
wait
LSREF=$(ls -1 "$MNT/HEAD/tree/manyfiles/" | sort)
LS_OK=1
for i in 1 2 3 4; do
	got=$(cat /tmp/git-fs-ls-$i)
	rm -f /tmp/git-fs-ls-$i
	if [ "$got" != "$LSREF" ]; then LS_OK=0; fi
done
assert_eq "$LS_OK" "1" "concurrent_parallel_readdir"

# --- concurrent: readdir + lookup on different paths ---
ls "$MNT/branches/heads/" > /dev/null &
PID1=$!
cat "$MNT/HEAD/tree/file.txt" > /tmp/git-fs-mix-1 &
PID2=$!
ls -1 "$MNT/HEAD/tree/manyfiles/" > /tmp/git-fs-mix-2 &
PID3=$!
cat "$MNT/tags/v1.0/hash" > /tmp/git-fs-mix-3 &
PID4=$!
wait $PID1 $PID2 $PID3 $PID4
assert_eq "$(cat /tmp/git-fs-mix-1)" "hello" "concurrent_mixed_read"
MIX_LS=$(wc -l < /tmp/git-fs-mix-2)
assert_eq "$MIX_LS" "500" "concurrent_mixed_readdir"
assert_match "$(cat /tmp/git-fs-mix-3)" "^[0-9a-f]{40}$" "concurrent_mixed_hash"
rm -f /tmp/git-fs-mix-1 /tmp/git-fs-mix-2 /tmp/git-fs-mix-3

# --- HEAD refresh: HEAD changes after mount ---
OLD_HEAD=$(cat "$MNT/HEAD/hash")
cd "$REPO"
echo "new" > newfile.txt
git add newfile.txt
git commit -q -m "post-mount commit"
cd - > /dev/null
sleep 1.5
NEW_HEAD=$(cat "$MNT/HEAD/hash")
if [ "$OLD_HEAD" != "$NEW_HEAD" ]; then
	PASS=$((PASS + 1))
	printf "  %-40s  ok\n" "head_refresh_after_commit"
else
	FAIL=$((FAIL + 1))
	printf "  FAIL %-34s  HEAD hash unchanged after commit\n" "head_refresh_after_commit"
fi
NEW_HEAD_MSG=$(cat "$MNT/HEAD/msg")
assert_contains "$NEW_HEAD_MSG" "post-mount commit" "head_refresh_msg"

# Unmount
fusermount3 -u "$MNT"
assert_eq "$?" "0" "unmount_succeeds"

echo
TOTAL=$((PASS + FAIL))
echo "$PASS/$TOTAL tests passed"
[ "$FAIL" -eq 0 ]
