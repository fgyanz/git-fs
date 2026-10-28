#!/bin/sh
# Integration tests for git-fs
# Usage: ./tests/test_mount.sh [/path/to/git-fs-binary]

set -e

. "$(dirname "$0")/lib.sh"
use_gitfs "$1"

wait_for_entry() {
	dir="$1"
	want="$2"
	for _ in 1 2 3 4 5 6 7 8 9 10; do
		if ls "$dir" 2>/dev/null | grep -q "$want"; then
			return 0
		fi
		sleep 0.1
	done
	return 1
}

REPO="$TESTDIR/repo"
MNT="$TESTDIR/mnt"
REMOTE="$TESTDIR/remote"
mkdir "$MNT"
new_repo "$REPO"

cd "$REPO"
echo "hello" > file.txt
mkdir subdir
echo "nested" > subdir/inner.txt
git add .
GIT_COMMITTER_DATE="@1700000000 +0000" git commit -q -m "first commit"
echo "world" > file2.txt
git add file2.txt
GIT_COMMITTER_DATE="@1700000100 +0000" git commit -q -m "second commit"
# 500 entries force several readdir calls
mkdir manyfiles
for i in $(seq -w 1 500); do
	echo "content-$i" > "manyfiles/f-$i.txt"
done
git add manyfiles
GIT_COMMITTER_DATE="@1700000200 +0000" git commit -q -m "add large directory"
git tag v1.0
git branch feature

git clone -q --bare "$REPO" "$REMOTE"
git remote add origin "$REMOTE"
git fetch -q origin

cd - > /dev/null

echo "test_mount:"

(cd "$REPO" && "$GITFS" -m "$MNT")
wait_mount "$MNT" || true

ROOT_LS=$(ls "$MNT")
assert_contains "$ROOT_LS" "HEAD" "readdir_root_HEAD"
assert_contains "$ROOT_LS" "branches" "readdir_root_branches"
assert_contains "$ROOT_LS" "tags" "readdir_root_tags"
assert_contains "$ROOT_LS" "objects" "readdir_root_objects"

COMMIT_LS=$(ls "$MNT/HEAD")
assert_contains "$COMMIT_LS" "tree" "readdir_commit_tree"
assert_contains "$COMMIT_LS" "hash" "readdir_commit_hash"
assert_contains "$COMMIT_LS" "msg" "readdir_commit_msg"
assert_contains "$COMMIT_LS" "parent" "readdir_commit_parent"

TREE_LS=$(ls "$MNT/HEAD/tree/")
assert_contains "$TREE_LS" "file.txt" "readdir_tree_file"
assert_contains "$TREE_LS" "file2.txt" "readdir_tree_file2"
assert_contains "$TREE_LS" "subdir" "readdir_tree_subdir"
assert_contains "$TREE_LS" "manyfiles" "readdir_tree_manyfiles"

SUB_LS=$(ls "$MNT/HEAD/tree/subdir/")
assert_contains "$SUB_LS" "inner.txt" "readdir_subdir_file"

BR_LS=$(ls "$MNT/branches/heads/")
assert_contains "$BR_LS" "feature" "readdir_branches_feature"

TAG_LS=$(ls "$MNT/tags/")
assert_contains "$TAG_LS" "v1.0" "readdir_tags_v1.0"

OBJ_LS=$(ls "$MNT/objects/")
assert_match "$OBJ_LS" "^[0-9a-f]{40}$" "readdir_objects_hex_entries"
OBJ_COUNT=$(echo "$OBJ_LS" | wc -l)
assert_eq "$OBJ_COUNT" "3" "readdir_objects_count"

HEAD_SHA=$(cd "$REPO" && git rev-parse HEAD)
OBJ_COMMIT_LS=$(ls "$MNT/objects/$HEAD_SHA/")
assert_contains "$OBJ_COMMIT_LS" "tree" "objects_commit_has_tree"
assert_contains "$OBJ_COMMIT_LS" "hash" "objects_commit_has_hash"
assert_contains "$OBJ_COMMIT_LS" "msg" "objects_commit_has_msg"
assert_contains "$OBJ_COMMIT_LS" "parent" "objects_commit_has_parent"

OBJ_HASH=$(cat "$MNT/objects/$HEAD_SHA/hash")
assert_eq "$OBJ_HASH" "$HEAD_SHA" "objects_hash_matches_name"

OBJ_MSG=$(cat "$MNT/objects/$HEAD_SHA/msg")
assert_contains "$OBJ_MSG" "add large directory" "objects_msg_readable"

OBJ_TREE_LS=$(ls "$MNT/objects/$HEAD_SHA/tree/")
assert_contains "$OBJ_TREE_LS" "file.txt" "objects_tree_has_files"

PARENT_SHA=$(cd "$REPO" && git rev-parse HEAD~1)
OBJ_PARENT_HASH=$(cat "$MNT/objects/$HEAD_SHA/parent/hash")
assert_eq "$OBJ_PARENT_HASH" "$PARENT_SHA" "objects_parent_chain"

ROOT_SHA=$(cd "$REPO" && git rev-parse HEAD~2)
OBJ_ROOT_LS=$(ls "$MNT/objects/$ROOT_SHA/")
assert_contains "$OBJ_ROOT_LS" "tree" "objects_root_commit_tree"
assert_contains "$OBJ_ROOT_LS" "msg" "objects_root_commit_msg"
assert_no_match "$OBJ_ROOT_LS" "^parent$" "objects_root_no_parent"

assert_fail "ls '$MNT/objects/not-a-hash'" "objects_invalid_hash"
assert_fail "ls '$MNT/objects/0000000000000000000000000000000000000000'" "objects_nonexistent_hash"

STAT_OBJ=$(stat -c '%F' "$MNT/objects/$HEAD_SHA")
assert_eq "$STAT_OBJ" "directory" "objects_commit_is_dir"

HASH=$(cat "$MNT/HEAD/hash")
assert_match "$HASH" "^[0-9a-f]{40}$" "read_hash_valid_hex"

MSG=$(cat "$MNT/HEAD/msg")
assert_contains "$MSG" "add large directory" "read_msg_content"

FILE_CONTENT=$(cat "$MNT/HEAD/tree/file.txt")
assert_eq "$FILE_CONTENT" "hello" "read_blob_content"

NESTED=$(cat "$MNT/HEAD/tree/subdir/inner.txt")
assert_eq "$NESTED" "nested" "read_nested_blob"

TAG_MSG=$(cat "$MNT/tags/v1.0/msg")
assert_contains "$TAG_MSG" "add large directory" "read_tag_msg"

PARENT_MSG=$(cat "$MNT/HEAD/parent/msg")
assert_contains "$PARENT_MSG" "second commit" "read_parent_msg"

STAT_ROOT=$(stat -c '%F' "$MNT")
assert_eq "$STAT_ROOT" "directory" "getattr_root_is_dir"

STAT_HEAD=$(stat -c '%F' "$MNT/HEAD")
assert_eq "$STAT_HEAD" "directory" "getattr_HEAD_is_dir"

STAT_TREE=$(stat -c '%F' "$MNT/HEAD/tree")
assert_eq "$STAT_TREE" "directory" "getattr_tree_is_dir"

STAT_SUB=$(stat -c '%F' "$MNT/HEAD/tree/subdir")
assert_eq "$STAT_SUB" "directory" "getattr_subdir_is_dir"

STAT_FILE=$(stat -c '%F' "$MNT/HEAD/tree/file.txt")
assert_eq "$STAT_FILE" "regular file" "getattr_file_is_regular"

STAT_HASH=$(stat -c '%F' "$MNT/HEAD/hash")
assert_eq "$STAT_HASH" "regular file" "getattr_hash_is_regular"

STAT_SIZE=$(stat -c '%s' "$MNT/HEAD/tree/file.txt")
assert_eq "$STAT_SIZE" "6" "getattr_file_size"

STAT_HASH_SIZE=$(stat -c '%s' "$MNT/HEAD/hash")
assert_eq "$STAT_HASH_SIZE" "41" "getattr_hash_size"

STAT_NLINK=$(stat -c '%h' "$MNT")
assert_match "$STAT_NLINK" "^[2-9]" "getattr_dir_nlink"

STAT_FNLINK=$(stat -c '%h' "$MNT/HEAD/hash")
assert_eq "$STAT_FNLINK" "1" "getattr_file_nlink"

assert_fail "cat '$MNT/nonexistent' " "lookup_enoent"
assert_fail "ls '$MNT/HEAD/nonexistent'" "lookup_commit_enoent"
assert_fail "cat '$MNT/HEAD/tree/no-such-file'" "lookup_tree_enoent"

assert_fail "touch '$MNT/newfile'" "write_readonly"
assert_fail "mkdir '$MNT/newdir'" "mkdir_readonly"
assert_fail "rm '$MNT/HEAD/hash'" "rm_readonly"

# HEAD/parent/parent is the root commit
ROOT_COMMIT_LS=$(ls "$MNT/HEAD/parent/parent/")
assert_contains "$ROOT_COMMIT_LS" "tree" "root_commit_has_tree"
assert_contains "$ROOT_COMMIT_LS" "hash" "root_commit_has_hash"
assert_contains "$ROOT_COMMIT_LS" "msg" "root_commit_has_msg"
assert_no_match "$ROOT_COMMIT_LS" "^parent$" "root_commit_no_parent"

MANY_COUNT=$(ls -1 "$MNT/HEAD/tree/manyfiles/" | wc -l)
assert_eq "$MANY_COUNT" "500" "readdir_large_count"

FIRST=$(cat "$MNT/HEAD/tree/manyfiles/f-001.txt")
assert_eq "$FIRST" "content-001" "readdir_large_first"

MID=$(cat "$MNT/HEAD/tree/manyfiles/f-250.txt")
assert_eq "$MID" "content-250" "readdir_large_mid"

LAST=$(cat "$MNT/HEAD/tree/manyfiles/f-500.txt")
assert_eq "$LAST" "content-500" "readdir_large_last"

MANY_UNIQ=$(ls -1 "$MNT/HEAD/tree/manyfiles/" | sort -u | wc -l)
assert_eq "$MANY_UNIQ" "500" "readdir_large_no_dupes"

BRANCH_HASH=$(cat "$MNT/tags/v1.0/hash")
assert_match "$BRANCH_HASH" "^[0-9a-f]{40}$" "tag_hash_valid"

REMOTE_LS=$(ls "$MNT/branches/remotes/")
assert_contains "$REMOTE_LS" "origin" "readdir_remotes_origin"

REMOTE_BR_LS=$(ls "$MNT/branches/remotes/origin/")
assert_contains "$REMOTE_BR_LS" "master" "readdir_remote_branches_master"

REMOTE_HASH=$(cat "$MNT/branches/remotes/origin/master/hash")
HEAD_HASH=$(cat "$MNT/HEAD/hash")
assert_eq "$REMOTE_HASH" "$HEAD_HASH" "remote_branch_matches_HEAD"

cd "$REPO"
git branch newbranch
cd - > /dev/null
wait_for_entry "$MNT/branches/heads/" "newbranch" || true
NEW_BR_LS=$(ls "$MNT/branches/heads/")
assert_contains "$NEW_BR_LS" "newbranch" "ref_refresh_new_branch"

cd "$REPO"
git tag v2.0
cd - > /dev/null
wait_for_entry "$MNT/tags/" "v2.0" || true
NEW_TAG_LS=$(ls "$MNT/tags/")
assert_contains "$NEW_TAG_LS" "v2.0" "ref_refresh_new_tag"

for i in 1 2 3 4 5 6 7 8; do
	cat "$MNT/HEAD/tree/manyfiles/f-$(printf '%03d' $((i * 60))).txt" > $TESTDIR/par-$i &
done
wait
PAR_OK=1
for i in 1 2 3 4 5 6 7 8; do
	expected="content-$(printf '%03d' $((i * 60)))"
	got=$(cat $TESTDIR/par-$i)
	rm -f $TESTDIR/par-$i
	if [ "$got" != "$expected" ]; then PAR_OK=0; fi
done
assert_eq "$PAR_OK" "1" "concurrent_parallel_reads"

for i in 1 2 3 4; do
	ls -1 "$MNT/HEAD/tree/manyfiles/" | sort > $TESTDIR/ls-$i &
done
wait
LSREF=$(ls -1 "$MNT/HEAD/tree/manyfiles/" | sort)
LS_OK=1
for i in 1 2 3 4; do
	got=$(cat $TESTDIR/ls-$i)
	rm -f $TESTDIR/ls-$i
	if [ "$got" != "$LSREF" ]; then LS_OK=0; fi
done
assert_eq "$LS_OK" "1" "concurrent_parallel_readdir"

ls "$MNT/branches/heads/" > /dev/null &
PID1=$!
cat "$MNT/HEAD/tree/file.txt" > $TESTDIR/mix-1 &
PID2=$!
ls -1 "$MNT/HEAD/tree/manyfiles/" > $TESTDIR/mix-2 &
PID3=$!
cat "$MNT/tags/v1.0/hash" > $TESTDIR/mix-3 &
PID4=$!
wait $PID1 $PID2 $PID3 $PID4
assert_eq "$(cat $TESTDIR/mix-1)" "hello" "concurrent_mixed_read"
MIX_LS=$(wc -l < $TESTDIR/mix-2)
assert_eq "$MIX_LS" "500" "concurrent_mixed_readdir"
assert_match "$(cat $TESTDIR/mix-3)" "^[0-9a-f]{40}$" "concurrent_mixed_hash"
rm -f $TESTDIR/mix-1 $TESTDIR/mix-2 $TESTDIR/mix-3

HEAD_COMMIT_TIME=$(cd "$REPO" && git log -1 --format='%ct' HEAD)
STAT_MTIME=$(stat -c '%Y' "$MNT/HEAD/tree/file.txt")
assert_eq "$STAT_MTIME" "$HEAD_COMMIT_TIME" "mtime_file_matches_commit"

STAT_HASH_MTIME=$(stat -c '%Y' "$MNT/HEAD/hash")
assert_eq "$STAT_HASH_MTIME" "$HEAD_COMMIT_TIME" "mtime_hash_matches_commit"

STAT_MSG_MTIME=$(stat -c '%Y' "$MNT/HEAD/msg")
assert_eq "$STAT_MSG_MTIME" "$HEAD_COMMIT_TIME" "mtime_msg_matches_commit"

STAT_TREE_MTIME=$(stat -c '%Y' "$MNT/HEAD/tree")
assert_eq "$STAT_TREE_MTIME" "$HEAD_COMMIT_TIME" "mtime_tree_dir_matches_commit"

STAT_NESTED_MTIME=$(stat -c '%Y' "$MNT/HEAD/tree/subdir/inner.txt")
assert_eq "$STAT_NESTED_MTIME" "$HEAD_COMMIT_TIME" "mtime_nested_file_propagates"

STAT_SUBDIR_MTIME=$(stat -c '%Y' "$MNT/HEAD/tree/subdir")
assert_eq "$STAT_SUBDIR_MTIME" "$HEAD_COMMIT_TIME" "mtime_subdir_propagates"

PARENT_MTIME=$(stat -c '%Y' "$MNT/HEAD/parent")
PARENT_COMMIT_TIME=$(cd "$REPO" && git log -1 --format='%ct' HEAD~1)
assert_eq "$PARENT_MTIME" "$PARENT_COMMIT_TIME" "mtime_parent_matches_git"

TAG_COMMIT_TIME=$(cd "$REPO" && git log -1 --format='%ct' v1.0)
TAG_MTIME=$(stat -c '%Y' "$MNT/tags/v1.0")
assert_eq "$TAG_MTIME" "$TAG_COMMIT_TIME" "mtime_tag_matches_git"

FEATURE_TIME=$(cd "$REPO" && git log -1 --format='%ct' feature)
FEATURE_MTIME=$(stat -c '%Y' "$MNT/branches/heads/feature")
assert_eq "$FEATURE_MTIME" "$FEATURE_TIME" "mtime_branch_matches_git"

HEAD_DIR_MTIME=$(stat -c '%Y' "$MNT/HEAD")
assert_eq "$HEAD_DIR_MTIME" "$HEAD_COMMIT_TIME" "mtime_HEAD_dir_matches_commit"

ROOT_MTIME=$(stat -c '%Y' "$MNT")
assert_eq "$ROOT_MTIME" "0" "mtime_root_is_zero"

BRANCHES_MTIME=$(stat -c '%Y' "$MNT/branches")
assert_eq "$BRANCHES_MTIME" "0" "mtime_branches_is_zero"

TAGS_MTIME=$(stat -c '%Y' "$MNT/tags")
assert_eq "$TAGS_MTIME" "0" "mtime_tags_dir_is_zero"

STAT_ATIME=$(stat -c '%X' "$MNT/HEAD/tree/file.txt")
STAT_CTIME=$(stat -c '%Z' "$MNT/HEAD/tree/file.txt")
assert_eq "$STAT_ATIME" "$HEAD_COMMIT_TIME" "atime_matches_commit"
assert_eq "$STAT_CTIME" "$HEAD_COMMIT_TIME" "ctime_matches_commit"

STAT_BLKSIZE=$(stat -c '%o' "$MNT/HEAD/tree/file.txt")
assert_eq "$STAT_BLKSIZE" "4096" "blksize_is_4096"

STAT_BLOCKS=$(stat -c '%b' "$MNT/HEAD/tree/file.txt")
assert_eq "$STAT_BLOCKS" "1" "blocks_file"

STAT_HASH_BLOCKS=$(stat -c '%b' "$MNT/HEAD/hash")
assert_eq "$STAT_HASH_BLOCKS" "1" "blocks_hash"

STAT_DIR_BLOCKS=$(stat -c '%b' "$MNT/HEAD/tree")
assert_eq "$STAT_DIR_BLOCKS" "0" "blocks_dir_zero"

OLD_HEAD=$(cat "$MNT/HEAD/hash")
cd "$REPO"
echo "new" > newfile.txt
git add newfile.txt
git commit -q -m "post-mount commit"
cd - > /dev/null
for _ in 1 2 3 4 5 6 7 8 9 10; do
	NEW_HEAD=$(cat "$MNT/HEAD/hash")
	[ "$OLD_HEAD" != "$NEW_HEAD" ] && break
	sleep 0.1
done
assert_ne "$NEW_HEAD" "$OLD_HEAD" "head_refresh_after_commit"
NEW_HEAD_MSG=$(cat "$MNT/HEAD/msg")
assert_contains "$NEW_HEAD_MSG" "post-mount commit" "head_refresh_msg"

assert_ok "fusermount3 -u '$MNT'" "unmount_succeeds"

(cd "$REPO" && "$GITFS" -m "$MNT")
wait_mount "$MNT" || true
assert_ok "ls '$MNT/HEAD'" "remount_for_unmount_test"

assert_ok "'$GITFS' -u '$MNT'" "unmount_flag_short"
MOUNTED=0; grep -q "$MNT" /proc/mounts && MOUNTED=1
assert_eq "$MOUNTED" "0" "unmount_flag_short_verified"

(cd "$REPO" && "$GITFS" -m "$MNT")
wait_mount "$MNT" || true

assert_ok "'$GITFS' --unmount '$MNT'" "unmount_flag_long"
MOUNTED=0; grep -q "$MNT" /proc/mounts && MOUNTED=1
assert_eq "$MOUNTED" "0" "unmount_flag_long_verified"

BAD_RET=0; "$GITFS" -u /nonexistent/path 2>/dev/null || BAD_RET=$?
assert_eq "$BAD_RET" "1" "unmount_bad_path"

DEFAULT_MNT="${REPO}/.git/fs"
(cd "$REPO" && "$GITFS")
wait_mount "$DEFAULT_MNT" || true
assert_ok "ls '$DEFAULT_MNT/HEAD'" "unmount_default_setup"
assert_ok "cd '$REPO' && '$GITFS' -u" "unmount_no_path"
MOUNTED=0; grep -q "$DEFAULT_MNT" /proc/mounts && MOUNTED=1
assert_eq "$MOUNTED" "0" "unmount_no_path_verified"
rmdir "$DEFAULT_MNT" 2>/dev/null || true

NEW_MNT="$TESTDIR/new-mnt"
(cd "$REPO" && "$GITFS" -m "$NEW_MNT")
wait_mount "$NEW_MNT" || true
assert_eq "$(test -d "$NEW_MNT" && echo yes)" "yes" "mkdir_creates_mountpoint"
assert_ok "ls '$NEW_MNT/HEAD'" "mkdir_mount_works"
fusermount3 -u "$NEW_MNT"
rmdir "$NEW_MNT"

DEFAULT_MNT="${REPO}/.git/fs"
rm -rf "$DEFAULT_MNT"
(cd "$REPO" && "$GITFS")
wait_mount "$DEFAULT_MNT" || true
assert_eq "$(test -d "$DEFAULT_MNT" && echo yes)" "yes" "mkdir_default_created"
assert_ok "ls '$DEFAULT_MNT/HEAD'" "mkdir_default_mount_works"
fusermount3 -u "$DEFAULT_MNT"
rmdir "$DEFAULT_MNT"

EXIST_MNT="$TESTDIR/exist-mnt"
mkdir "$EXIST_MNT"
(cd "$REPO" && "$GITFS" -m "$EXIST_MNT")
wait_mount "$EXIST_MNT" || true
assert_ok "ls '$EXIST_MNT/HEAD'" "mkdir_existing_dir_works"
fusermount3 -u "$EXIST_MNT"
rmdir "$EXIST_MNT"

summary
