#!/bin/sh
# Stress test for git-fs data integrity under concurrent load
# Usage: ./tests/test_stress.sh [/path/to/git-fs-binary]
#
# Environment variables:
#   WORKERS          - concurrent reader processes (default: 16)
#   ROUNDS           - max rounds (default: 5)
#   REPO             - external repo path; empty = create test repo
#   MAX_COMMITS      - commits sampled from the repo (default: 20)
#   DURATION         - keep running rounds this long, e.g. 5m, 1h30m
#   RANDOM_SAMPLE    - 1 = re-sample commits and refs every round
#                      (default: 1 if DURATION is set)
#   CHURN            - 1 = move refs while reading: stress-churn refs
#                      and HEAD, restored on exit. Needs a writable REPO
#   CHURN_INTERVAL   - seconds between churn ops (default: 0.1)
#   MONITOR_INTERVAL - seconds between resource samples (default: 2)

set -e

. "$(dirname "$0")/lib.sh"
use_gitfs "$1"

WORKERS="${WORKERS:-16}"
ROUNDS="${ROUNDS:-5}"
EXT_REPO="${REPO:-}"
MAX_COMMITS="${MAX_COMMITS:-20}"
DURATION="${DURATION:-}"
CHURN="${CHURN:-0}"
CHURN_INTERVAL="${CHURN_INTERVAL:-0.1}"
if [ -z "${RANDOM_SAMPLE:-}" ]; then
	if [ -n "$DURATION" ]; then RANDOM_SAMPLE=1; else RANDOM_SAMPLE=0; fi
fi

# prints nothing if $1 can't be parsed
duration_to_seconds() {
	d=$1
	[ -z "$d" ] && return 0
	case "$d" in
		*[!0-9]*) ;;
		*) echo "$d"; return 0 ;;
	esac
	total=0
	rem=$d
	while [ -n "$rem" ]; do
		num=$(echo "$rem" | sed -n 's/^\([0-9][0-9]*\).*/\1/p')
		[ -z "$num" ] && return 1
		rest=${rem#$num}
		unit=$(echo "$rest" | cut -c1)
		case "$unit" in
			h) total=$((total + num * 3600)) ;;
			m) total=$((total + num * 60)) ;;
			s) total=$((total + num)) ;;
			*) return 1 ;;
		esac
		rem=$(echo "$rest" | cut -c2-)
	done
	echo "$total"
}

DURATION_SECS=""
if [ -n "$DURATION" ]; then
	DURATION_SECS=$(duration_to_seconds "$DURATION")
	if [ -z "$DURATION_SECS" ]; then
		echo "error: bad DURATION '$DURATION' (use e.g. 5m, 2h, 1h30m, 300s)" >&2
		exit 1
	fi
fi

MNT="$TESTDIR/mnt"
ERR_LOG="$TESTDIR/err.log"
WORK_DIR="$TESTDIR/work"
OWN_REPO=""
mkdir "$MNT" "$WORK_DIR"
: > "$ERR_LOG"

test_cleanup() {
	if [ -n "$monitor_pid" ]; then
		: > "$WORK_DIR/monitor-stop"
		wait "$monitor_pid" 2>/dev/null || true
	fi
	if [ "$CHURN" = "1" ]; then
		: > "$WORK_DIR/churner-stop" 2>/dev/null || true
		churn_cleanup 2>/dev/null || true
	fi
}

# verify_* FUSE_PATH SHA [REPO_PATH]: log failures to ERR_LOG and print
# 1 if the check passed, 0 if not

verify_blob() {
	ftmp=$(mktemp "$WORK_DIR/vfuse-XXXXXX")
	gtmp=$(mktemp "$WORK_DIR/vgit-XXXXXX")
	gerr=$(mktemp "$WORK_DIR/giterr-XXXXXX")

	if ! cat "$1" > "$ftmp" 2>/dev/null; then
		echo "FAIL blob $3 @ $2: fuse read failed path=$1" >> "$ERR_LOG"
		rm -f "$ftmp" "$gtmp" "$gerr"
		echo 0; return 0
	fi

	git -C "$REPO" show "$2:$3" > "$gtmp" 2>"$gerr"
	grc=$?
	if [ $grc -ne 0 ] || [ -s "$gerr" ]; then
		echo "FAIL blob $3 @ $2: git show failed (rc=$grc): $(cat "$gerr")" >> "$ERR_LOG"
		rm -f "$ftmp" "$gtmp" "$gerr"
		echo 0; return 0
	fi
	rm -f "$gerr"

	fsize=$(wc -c < "$ftmp")
	gsize=$(wc -c < "$gtmp")
	if [ "$fsize" != "$gsize" ]; then
		echo "FAIL blob $3 @ $2: size mismatch fuse=$fsize git=$gsize" >> "$ERR_LOG"
		rm -f "$ftmp" "$gtmp"
		echo 0; return 0
	fi

	fsum=$(sha256sum < "$ftmp" | cut -d' ' -f1)
	gsum=$(sha256sum < "$gtmp" | cut -d' ' -f1)
	rm -f "$ftmp" "$gtmp"

	if [ -z "$fsum" ] || [ -z "$gsum" ]; then
		echo "FAIL blob $3 @ $2: sha256sum failed fsum=$fsum gsum=$gsum" >> "$ERR_LOG"
		echo 0; return 0
	fi
	if [ "$fsum" != "$gsum" ]; then
		echo "FAIL blob $3 @ $2: fuse=$fsum git=$gsum" >> "$ERR_LOG"
		echo 0; return 0
	fi
	echo 1; return 0
}

verify_symlink() {
	# git stores symlinks as blobs: git-fs may show either
	gtarget=$(git -C "$REPO" show "$2:$3" 2>/dev/null) || {
		echo "FAIL symlink $3 @ $2: git show failed" >> "$ERR_LOG"
		echo 0; return 0
	}
	if [ -L "$1" ]; then
		ftarget=$(readlink "$1" 2>/dev/null || echo "READLINK_ERROR")
	elif [ -f "$1" ]; then
		ftarget=$(cat "$1" 2>/dev/null || echo "READ_ERROR")
	else
		echo "FAIL symlink $3 @ $2: not found at $1" >> "$ERR_LOG"
		echo 0; return 0
	fi
	if [ "$ftarget" != "$gtarget" ]; then
		echo "FAIL symlink $3 @ $2: fuse='$ftarget' git='$gtarget'" >> "$ERR_LOG"
		echo 0; return 0
	fi
	echo 1; return 0
}

verify_hash() {
	got=$(cat "$1" 2>/dev/null || echo "READ_ERROR")
	got=$(printf '%s' "$got" | tr -d '\n')
	if [ "$got" != "$2" ]; then
		echo "FAIL hash: fuse=$got expected=$2 path=$1" >> "$ERR_LOG"
		echo 0; return 0
	fi
	echo 1; return 0
}

verify_msg() {
	fmsg=$(cat "$1" 2>/dev/null || echo "READ_ERROR")
	gmsg=$(git -C "$REPO" log -1 --format='%B' "$2")
	if [ -z "$gmsg" ] && [ -z "$fmsg" ]; then
		echo "FAIL msg @ $2: both fuse and git returned empty, cannot verify path=$1" >> "$ERR_LOG"
		echo 0; return 0
	fi
	if [ "$fmsg" != "$gmsg" ]; then
		echo "FAIL msg @ $2: path=$1" >> "$ERR_LOG"
		echo 0; return 0
	fi
	echo 1; return 0
}

verify_tree_listing() {
	fls_err=$(mktemp "$WORK_DIR/lserr-XXXXXX")
	fls_raw=$(ls -1a "$1" 2>"$fls_err")
	fls_rc=$?
	if [ $fls_rc -ne 0 ]; then
		echo "FAIL readdir $3 @ $2: ls failed (rc=$fls_rc) on $1: $(cat "$fls_err")" >> "$ERR_LOG"
		rm -f "$fls_err"
		echo 0; return 0
	fi
	rm -f "$fls_err"
	fls=$(printf '%s\n' "$fls_raw" | grep -v '^\.\.\{0,1\}$' | grep -v '^$' | sort)
	gls=$(git -C "$REPO" ls-tree --name-only "$2" "$3/" | sed "s|^$3/||" | sort)
	if [ "$fls" != "$gls" ]; then
		[ -z "$fls" ] && fcnt=0 || fcnt=$(printf '%s\n' "$fls" | wc -l)
		[ -z "$gls" ] && gcnt=0 || gcnt=$(printf '%s\n' "$gls" | wc -l)

		ftmp2=$(mktemp "$WORK_DIR/fls-XXXXXX")
		gtmp2=$(mktemp "$WORK_DIR/gls-XXXXXX")
		printf '%s\n' "$fls" > "$ftmp2"
		printf '%s\n' "$gls" > "$gtmp2"
		echo "FAIL readdir $3 @ $2 (fuse=$fcnt git=$gcnt) path=$1" >> "$ERR_LOG"
		if [ "$fcnt" -eq 0 ]; then
			echo "  fuse returned NO entries; git has $gcnt:" >> "$ERR_LOG"
			head -5 "$gtmp2" | sed 's/^/    /' >> "$ERR_LOG"
		else
			echo "  diff (< git, > fuse):" >> "$ERR_LOG"
			diff "$gtmp2" "$ftmp2" | head -10 | sed 's/^/    /' >> "$ERR_LOG"
		fi
		rm -f "$ftmp2" "$gtmp2"
		echo 0; return 0
	fi
	echo 1; return 0
}

# count RESULT: bumps the calling worker's pass/fail
count() {
	if [ "$1" = "1" ]; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
	fi
}

write_counts() {
	echo "$pass" > "$cfile_p"
	echo "$fail" > "$cfile_f"
}

# workers run with set +e: a failed check must not end the worker

worker_tree_walk() {
	set +e
	wid=$1; sha=$2; round=$3
	pass=0; fail=0
	cfile_p="$WORK_DIR/pass-A-$round-$wid"
	cfile_f="$WORK_DIR/fail-A-$round-$wid"

	lstmp="$WORK_DIR/ls-A-$round-$wid.tmp"
	git -C "$REPO" ls-tree -r "$sha" > "$lstmp"
	while IFS='	' read -r meta path; do
		mode=$(echo "$meta" | cut -d' ' -f1)
		fpath="$MNT/objects/$sha/tree/$path"
		if [ "$mode" = "120000" ]; then
			count "$(verify_symlink "$fpath" "$sha" "$path")"
		elif [ "$mode" = "160000" ]; then
			# submodule: no blob to read
			continue
		else
			count "$(verify_blob "$fpath" "$sha" "$path")"
		fi
	done < "$lstmp"
	rm -f "$lstmp"

	count "$(verify_hash "$MNT/objects/$sha/hash" "$sha")"
	count "$(verify_msg "$MNT/objects/$sha/msg" "$sha")"
	write_counts
}

worker_contention() {
	set +e
	wid=$1; head_sha=$2; round=$3
	pass=0; fail=0
	cfile_p="$WORK_DIR/pass-B-$round-$wid"
	cfile_f="$WORK_DIR/fail-B-$round-$wid"

	big=$(git -C "$REPO" ls-tree -r -l "$head_sha" | sort -k4 -rn | sed '1q' | awk '{print $5}')
	small=$(git -C "$REPO" ls-tree -r --name-only "$head_sha" | sed '1q')

	# objects/$sha, not HEAD: HEAD may move under churn
	base="$MNT/objects/$head_sha/tree"
	i=0
	while [ $i -lt 50 ]; do
		if [ -n "$big" ]; then
			if [ -f "$base/$big" ]; then
				count "$(verify_blob "$base/$big" "$head_sha" "$big")"
			else
				echo "FAIL blob $big @ $head_sha: ENOENT on objects/ path" >> "$ERR_LOG"
				count 0
			fi
		fi
		if [ -n "$small" ]; then
			if [ -f "$base/$small" ]; then
				count "$(verify_blob "$base/$small" "$head_sha" "$small")"
			else
				echo "FAIL blob $small @ $head_sha: ENOENT on objects/ path" >> "$ERR_LOG"
				count 0
			fi
		fi
		count "$(verify_hash "$MNT/objects/$head_sha/hash" "$head_sha")"
		i=$((i + 1))
	done
	write_counts
}

worker_readdir_read() {
	set +e
	wid=$1; round=$2
	pass=0; fail=0
	cfile_p="$WORK_DIR/pass-C-$round-$wid"
	cfile_f="$WORK_DIR/fail-C-$round-$wid"

	while IFS= read -r sha; do
		dir=$(git -C "$REPO" ls-tree "$sha" | awk '$2=="tree"{print $4; exit}')
		[ -z "$dir" ] && continue
		[ -d "$MNT/objects/$sha/tree/$dir" ] || continue

		count "$(verify_tree_listing "$MNT/objects/$sha/tree/$dir" "$sha" "$dir")"

		flist="$WORK_DIR/flist-C-$round-$wid.tmp"
		ls -1 "$MNT/objects/$sha/tree/$dir/" > "$flist" 2>/dev/null
		nread=0
		while IFS= read -r f && [ $nread -lt 20 ]; do
			[ -f "$MNT/objects/$sha/tree/$dir/$f" ] || continue
			count "$(verify_blob "$MNT/objects/$sha/tree/$dir/$f" "$sha" "$dir/$f")"
			nread=$((nread + 1))
		done < "$flist"
		rm -f "$flist"
	done < "$WORK_DIR/commits.txt"
	write_counts
}

# reads HEAD/hash once, then only objects/$sha paths: under churn
# HEAD/* may already point at another commit
worker_parent_chain() {
	set +e
	wid=$1; round=$2
	pass=0; fail=0
	cfile_p="$WORK_DIR/pass-D-$round-$wid"
	cfile_f="$WORK_DIR/fail-D-$round-$wid"

	head_sha=$(cat "$MNT/HEAD/hash" 2>/dev/null | tr -d '\n')
	if [ -z "$head_sha" ]; then
		echo "FAIL parent chain: cannot read MNT/HEAD/hash" >> "$ERR_LOG"
		count 0; write_counts; return
	fi
	if ! git -C "$REPO" cat-file -e "${head_sha}^{commit}" 2>/dev/null; then
		echo "FAIL parent chain: FUSE HEAD '$head_sha' is not a commit" >> "$ERR_LOG"
		count 0; write_counts; return
	fi
	count "$(verify_msg "$MNT/objects/$head_sha/msg" "$head_sha")"

	chaintmp="$WORK_DIR/chain-D-$round-$wid.tmp"
	sha=$head_sha
	depth=0
	while [ $depth -lt 25 ] && [ -n "$sha" ]; do
		echo "$sha" >> "$chaintmp"
		sha=$(git -C "$REPO" log -1 --format='%P' "$sha" | cut -d' ' -f1)
		depth=$((depth + 1))
	done

	expected_depth=$(wc -l < "$chaintmp")
	cpath="$MNT/objects/$head_sha"
	depth=0
	while IFS= read -r expected_sha; do
		if [ ! -d "$cpath" ] || [ ! -f "$cpath/hash" ]; then
			echo "FAIL parent chain depth=$depth: fuse path missing $cpath" >> "$ERR_LOG"
			count 0
			break
		fi
		count "$(verify_hash "$cpath/hash" "$expected_sha")"
		count "$(verify_msg "$cpath/msg" "$expected_sha")"

		depth=$((depth + 1))
		if [ -d "$cpath/parent" ]; then
			cpath="$cpath/parent"
		else
			break
		fi
	done < "$chaintmp"
	rm -f "$chaintmp"

	if [ "$depth" -lt "$expected_depth" ]; then
		echo "FAIL parent chain: walked $depth of $expected_depth commits" >> "$ERR_LOG"
		count 0
	fi
	write_counts
}

# a ref may move under churn, so only check that the SHA it shows is a
# real commit, then read that commit's data through objects/$sha
worker_refs() {
	set +e
	wid=$1; round=$2
	pass=0; fail=0
	cfile_p="$WORK_DIR/pass-E-$round-$wid"
	cfile_f="$WORK_DIR/fail-E-$round-$wid"

	brtmp="$WORK_DIR/br-E-$round-$wid.tmp"
	if [ "$RANDOM_SAMPLE" = "1" ] && command -v shuf > /dev/null 2>&1; then
		git -C "$REPO" for-each-ref --format='%(refname:short)' refs/heads/ \
			| shuf -n 10 > "$brtmp"
	else
		git -C "$REPO" for-each-ref --format='%(refname:short)' refs/heads/ \
			--count=10 > "$brtmp"
	fi
	while IFS= read -r br; do
		case "$br" in stress-churn/*) continue ;; esac
		brname=$(echo "$br" | sed 's|.*/||')
		bpath="$MNT/branches/heads/$brname"
		if [ ! -d "$bpath" ]; then
			# deleted by the churner
			continue
		fi
		captured=$(cat "$bpath/hash" 2>/dev/null | tr -d '\n')
		if [ -z "$captured" ]; then
			echo "FAIL branch $brname: cannot read $bpath/hash" >> "$ERR_LOG"
			count 0; continue
		fi
		if ! git -C "$REPO" cat-file -e "${captured}^{commit}" 2>/dev/null; then
			echo "FAIL branch $brname: FUSE hash '$captured' is not a commit" >> "$ERR_LOG"
			count 0; continue
		fi
		count "$(verify_msg "$MNT/objects/$captured/msg" "$captured")"
		sample=$(git -C "$REPO" ls-tree -r --name-only "$captured" 2>/dev/null | sed '1q')
		if [ -n "$sample" ] && [ -f "$MNT/objects/$captured/tree/$sample" ]; then
			count "$(verify_blob "$MNT/objects/$captured/tree/$sample" "$captured" "$sample")"
		fi
	done < "$brtmp"
	rm -f "$brtmp"

	tagtmp="$WORK_DIR/tag-E-$round-$wid.tmp"
	if [ "$RANDOM_SAMPLE" = "1" ] && command -v shuf > /dev/null 2>&1; then
		git -C "$REPO" tag -l | shuf -n 10 > "$tagtmp"
	else
		git -C "$REPO" tag -l | head -10 > "$tagtmp"
	fi
	while IFS= read -r tag; do
		case "$tag" in stress-churn-*) continue ;; esac
		tpath="$MNT/tags/$tag"
		if [ ! -d "$tpath" ]; then
			# deleted by the churner
			continue
		fi
		captured=$(cat "$tpath/hash" 2>/dev/null | tr -d '\n')
		if [ -z "$captured" ]; then
			echo "FAIL tag $tag: cannot read $tpath/hash" >> "$ERR_LOG"
			count 0; continue
		fi
		if ! git -C "$REPO" cat-file -e "${captured}^{commit}" 2>/dev/null; then
			echo "FAIL tag $tag: FUSE hash '$captured' is not a commit" >> "$ERR_LOG"
			count 0; continue
		fi
		count 1
	done < "$tagtmp"
	rm -f "$tagtmp"

	write_counts
}

# three readers share one open file: the kernel serializes their
# read()s on the shared offset, so the slices must add up to the file.
# dd, not cat: GNU cat 9.11 sometimes reads a shared fd twice, even on
# local filesystems.
worker_same_fh() {
	set +e
	wid=$1; sha=$2; round=$3
	pass=0; fail=0
	cfile_p="$WORK_DIR/pass-F-$round-$wid"
	cfile_f="$WORK_DIR/fail-F-$round-$wid"

	# 64K-1M: several FUSE_READs, still cheap
	path=$(git -C "$REPO" ls-tree -r -l "$sha" 2>/dev/null \
		| awk '$4 > 65536 && $4 < 1048576 {print substr($0, index($0, $5)); exit}')
	if [ -z "$path" ]; then
		path=$(git -C "$REPO" ls-tree -r -l "$sha" 2>/dev/null \
			| awk '$4 > 0 {print substr($0, index($0, $5)); exit}')
	fi
	if [ -z "$path" ]; then
		write_counts; return
	fi

	fpath="$MNT/objects/$sha/tree/$path"
	if [ ! -f "$fpath" ]; then
		echo "FAIL same-fh $path @ $sha: ENOENT $fpath" >> "$ERR_LOG"
		count 0; write_counts; return
	fi

	expected_size=$(wc -c < "$fpath")
	if [ -z "$expected_size" ] || [ "$expected_size" -le 0 ]; then
		write_counts; return
	fi

	i=0
	while [ $i -lt 20 ]; do
		out1="$WORK_DIR/F-$round-$wid-$i-1"
		out2="$WORK_DIR/F-$round-$wid-$i-2"
		out3="$WORK_DIR/F-$round-$wid-$i-3"

		exec 3< "$fpath"
		dd bs=128K status=none <&3 > "$out1" &
		dd bs=128K status=none <&3 > "$out2" &
		dd bs=128K status=none <&3 > "$out3" &
		wait
		exec 3<&-

		s1=$(wc -c < "$out1")
		s2=$(wc -c < "$out2")
		s3=$(wc -c < "$out3")
		total=$((s1 + s2 + s3))
		rm -f "$out1" "$out2" "$out3"

		if [ "$total" != "$expected_size" ]; then
			echo "FAIL same-fh $path @ $sha: total=$total expected=$expected_size (slices=$s1,$s2,$s3)" >> "$ERR_LOG"
			fail=$((fail + 1))
		else
			pass=$((pass + 1))
		fi
		i=$((i + 1))
	done
	write_counts
}

CHURN_HEAD_BACKUP=""

churn_setup() {
	CHURN_HEAD_BACKUP=$(git -C "$REPO" rev-parse HEAD 2>/dev/null)
}

churn_cleanup() {
	[ -z "$CHURN_HEAD_BACKUP" ] && return
	git -C "$REPO" update-ref HEAD "$CHURN_HEAD_BACKUP" 2>/dev/null || true
	git -C "$REPO" for-each-ref --format='%(refname)' \
		refs/heads/stress-churn/ refs/tags/ 2>/dev/null \
		| while IFS= read -r r; do
			case "$r" in
				refs/heads/stress-churn/*|refs/tags/stress-churn-*)
					git -C "$REPO" update-ref -d "$r" 2>/dev/null || true
					;;
			esac
		done
	git -C "$REPO" pack-refs --all 2>/dev/null || true
	CHURN_HEAD_BACKUP=""
}

run_churner() {
	set +e
	round=$1
	next_id=0

	while [ ! -f "$WORK_DIR/churner-stop" ]; do
		op=$(printf '%s\n' add-br del-br add-tag del-tag move-head pack-refs \
		     | shuf -n 1 2>/dev/null)
		[ -z "$op" ] && op=add-br

		case "$op" in
			add-br)
				rsha=$(shuf -n 1 "$ALL_COMMITS" 2>/dev/null)
				[ -n "$rsha" ] && git -C "$REPO" update-ref \
					"refs/heads/stress-churn/r${round}-${next_id}" \
					"$rsha" 2>/dev/null
				next_id=$((next_id + 1))
				;;
			del-br)
				rb=$(git -C "$REPO" for-each-ref --format='%(refname)' \
				     refs/heads/stress-churn/ 2>/dev/null \
				     | shuf -n 1 2>/dev/null)
				[ -n "$rb" ] && git -C "$REPO" update-ref -d "$rb" 2>/dev/null
				;;
			add-tag)
				rsha=$(shuf -n 1 "$ALL_COMMITS" 2>/dev/null)
				[ -n "$rsha" ] && git -C "$REPO" update-ref \
					"refs/tags/stress-churn-r${round}-${next_id}" \
					"$rsha" 2>/dev/null
				next_id=$((next_id + 1))
				;;
			del-tag)
				rt=$(git -C "$REPO" for-each-ref --format='%(refname)' refs/tags/ 2>/dev/null \
				     | grep '^refs/tags/stress-churn-' \
				     | shuf -n 1 2>/dev/null)
				[ -n "$rt" ] && git -C "$REPO" update-ref -d "$rt" 2>/dev/null
				;;
			move-head)
				rsha=$(shuf -n 1 "$ALL_COMMITS" 2>/dev/null)
				[ -n "$rsha" ] && git -C "$REPO" update-ref HEAD "$rsha" 2>/dev/null
				;;
			pack-refs)
				git -C "$REPO" pack-refs --all 2>/dev/null
				;;
		esac
		sleep "$CHURN_INTERVAL"
	done
}

setup_repo() {
	REPO="$TESTDIR/repo"
	OWN_REPO="$REPO"
	new_repo "$REPO"

	mkdir -p "$REPO/a/b/c/d/e"
	echo "deep content" > "$REPO/a/b/c/d/e/f.txt"
	mkdir "$REPO/wide"
	i=1
	while [ $i -le 200 ]; do
		printf 'wide-content-%03d' "$i" > "$REPO/wide/f-$(printf '%03d' $i).txt"
		i=$((i + 1))
	done
	dd if=/dev/urandom bs=256 count=1 of="$REPO/bin-small.dat" 2>/dev/null
	dd if=/dev/urandom bs=4096 count=1 of="$REPO/bin-medium.dat" 2>/dev/null
	dd if=/dev/urandom bs=4096 count=16 of="$REPO/bin-large.dat" 2>/dev/null
	dd if=/dev/urandom bs=4096 count=256 of="$REPO/bin-huge.dat" 2>/dev/null
	: > "$REPO/empty.txt"
	(cd "$REPO" && ln -s a/b/c/d/e/f.txt link-to-f.txt)
	commit_all "$REPO" "commit 1: initial structure"
	git -C "$REPO" tag v0.1

	i=2
	while [ $i -le 20 ]; do
		echo "revision $i" > "$REPO/a/b/c/d/e/f.txt"
		echo "new-file-$i" > "$REPO/file-$i.txt"
		commit_all "$REPO" "commit $i"
		case $i in
			5)  git -C "$REPO" branch br-1; git -C "$REPO" tag v0.2 ;;
			10) git -C "$REPO" branch br-2; git -C "$REPO" tag v0.3 ;;
			15) git -C "$REPO" branch br-3; git -C "$REPO" tag v0.4 ;;
			20) git -C "$REPO" branch br-4 ;;
		esac
		i=$((i + 1))
	done
}

run_workers() {
	round=$1
	head_sha=$(git -C "$REPO" rev-parse HEAD)
	num_commits=$(wc -l < "$WORK_DIR/commits.txt")

	if [ "$num_commits" -eq 0 ]; then
		echo "error: no commits found in repo" >&2
		exit 1
	fi

	wa=$((WORKERS * 22 / 100))
	wb=$((WORKERS * 22 / 100))
	wc_n=$((WORKERS * 22 / 100))
	wd=$((WORKERS / 10))
	we=$((WORKERS / 10))
	wf=$((WORKERS - wa - wb - wc_n - wd - we))
	[ $wa -lt 1 ] && wa=1
	[ $wb -lt 1 ] && wb=1
	[ $wc_n -lt 1 ] && wc_n=1
	[ $wd -lt 1 ] && wd=1
	[ $we -lt 1 ] && we=1
	[ $wf -lt 1 ] && wf=1

	wid=0
	reader_pids=""

	if [ "$CHURN" = "1" ]; then
		run_churner "$round" &
		churner_pid=$!
	fi

	i=0
	while [ $i -lt $wa ]; do
		line=$(( (wid % num_commits) + 1 ))
		sha=$(sed -n "${line}p" "$WORK_DIR/commits.txt")
		worker_tree_walk "$wid" "$sha" "$round" &
		reader_pids="$reader_pids $!"
		wid=$((wid + 1))
		i=$((i + 1))
	done

	i=0
	while [ $i -lt $wb ]; do
		worker_contention "$wid" "$head_sha" "$round" &
		reader_pids="$reader_pids $!"
		wid=$((wid + 1))
		i=$((i + 1))
	done

	i=0
	while [ $i -lt $wc_n ]; do
		worker_readdir_read "$wid" "$round" &
		reader_pids="$reader_pids $!"
		wid=$((wid + 1))
		i=$((i + 1))
	done

	i=0
	while [ $i -lt $wd ]; do
		worker_parent_chain "$wid" "$round" &
		reader_pids="$reader_pids $!"
		wid=$((wid + 1))
		i=$((i + 1))
	done

	i=0
	while [ $i -lt $we ]; do
		worker_refs "$wid" "$round" &
		reader_pids="$reader_pids $!"
		wid=$((wid + 1))
		i=$((i + 1))
	done

	i=0
	while [ $i -lt $wf ]; do
		line=$(( (wid % num_commits) + 1 ))
		sha=$(sed -n "${line}p" "$WORK_DIR/commits.txt")
		worker_same_fh "$wid" "$sha" "$round" &
		reader_pids="$reader_pids $!"
		wid=$((wid + 1))
		i=$((i + 1))
	done

	# not a bare wait: the churner runs until told to stop
	for pid in $reader_pids; do
		wait "$pid" 2>/dev/null
	done

	if [ "$CHURN" = "1" ]; then
		: > "$WORK_DIR/churner-stop"
		wait "$churner_pid" 2>/dev/null
		rm -f "$WORK_DIR/churner-stop"
	fi
}

run_mutation_phase() {
	echo "  mutation: starting background readers..."
	mut_pass=0
	mut_fail=0

	bg_pids=""
	num_commits=$(wc -l < "$WORK_DIR/commits.txt")
	i=0
	while [ $i -lt 8 ] && [ $i -lt "$WORKERS" ]; do
		line=$(( (i % num_commits) + 1 ))
		sha=$(sed -n "${line}p" "$WORK_DIR/commits.txt")
		worker_tree_walk "mut-$i" "$sha" "mut" &
		bg_pids="$bg_pids $!"
		i=$((i + 1))
	done

	i=1
	while [ $i -le 5 ]; do
		echo "mutation $i" > "$REPO/mutated-$i.txt"
		git -C "$REPO" add "mutated-$i.txt"
		git -C "$REPO" commit -q -m "mutation commit $i"
		i=$((i + 1))
	done
	git -C "$REPO" branch mutation-branch

	# let the inotify watcher catch up
	sleep 2

	new_head=$(cat "$MNT/HEAD/hash")
	new_head=$(printf '%s' "$new_head" | tr -d '\n')
	actual_head=$(git -C "$REPO" rev-parse HEAD)
	if [ "$new_head" != "$actual_head" ]; then
		echo "FAIL mutation: HEAD not updated: fuse=$new_head git=$actual_head" >> "$ERR_LOG"
		mut_fail=$((mut_fail + 1))
	else
		mut_pass=$((mut_pass + 1))
	fi

	if ls "$MNT/branches/heads/" | grep -q "mutation-branch"; then
		mut_pass=$((mut_pass + 1))
	else
		echo "FAIL mutation: mutation-branch not visible" >> "$ERR_LOG"
		mut_fail=$((mut_fail + 1))
	fi

	r=$(verify_blob "$MNT/HEAD/tree/mutated-5.txt" "$actual_head" "mutated-5.txt")
	if [ "$r" = "1" ]; then
		mut_pass=$((mut_pass + 1))
	else
		mut_fail=$((mut_fail + 1))
	fi

	echo "$mut_pass" > "$WORK_DIR/pass-mut-checks"
	echo "$mut_fail" > "$WORK_DIR/fail-mut-checks"

	for pid in $bg_pids; do
		wait "$pid" 2>/dev/null || true
	done
}

aggregate() {
	total_pass=0
	total_fail=0
	for f in "$WORK_DIR"/pass-*; do
		[ -f "$f" ] && total_pass=$((total_pass + $(cat "$f")))
	done
	for f in "$WORK_DIR"/fail-*; do
		[ -f "$f" ] && total_fail=$((total_fail + $(cat "$f")))
	done
	echo "$total_pass" "$total_fail"
}

MONITOR_INTERVAL="${MONITOR_INTERVAL:-2}"
STATE_LOG=""
monitor_pid=""

run_monitor() {
	pid=$GITFS_PID
	echo "monitor: tracking pid=$pid (interval=${MONITOR_INTERVAL}s)" >> "$STATE_LOG"
	t0=$(date +%s)
	while [ ! -f "$WORK_DIR/monitor-stop" ]; do
		if [ ! -d "/proc/$pid" ]; then
			now=$(date +%s)
			echo "[T+$((now - t0))s] DIED — /proc/$pid gone" >> "$STATE_LOG"
			return
		fi
		now=$(date +%s)
		echo "[T+$((now - t0))s] $(res_fds "$pid") $(res_mem "$pid")" \
			>> "$STATE_LOG"
		sleep "$MONITOR_INTERVAL"
	done
}

print_resource_summary() {
	samples="$WORK_DIR/samples"
	grep 'total=' "$STATE_LOG" > "$samples" 2>/dev/null || true
	[ -s "$samples" ] || return 0
	echo "  resources (start / end / peak over $(wc -l < "$samples") samples):"
	for f in total memfd memfd_bytes pack rss anon file threads; do
		printf "    %-12s %s / %s / %s\n" "$f" "$(res_first "$samples" $f)" \
			"$(res_last "$samples" $f)" "$(res_peak "$samples" $f)"
	done
}

print_gitfs_diagnostics() {
	echo ""
	echo "=== git-fs process state at time of failure ==="
	pid=$GITFS_PID
	if [ ! -d "/proc/$pid" ]; then
		echo "(no git-fs pid found — process is gone)"
	else
		echo "pid=$pid"
		grep -E 'State|Threads|VmSize|VmRSS|VmHWM|VmPeak|Rss|voluntary_ctxt|nonvoluntary' \
			"/proc/$pid/status" 2>/dev/null || echo "(/proc/$pid/status unreadable)"
		echo "fds: $(res_fds "$pid")"
		echo "--- /proc/$pid/smaps_rollup ---"
		head -8 "/proc/$pid/smaps_rollup" 2>/dev/null || echo "(unreadable)"
	fi
	echo ""
	echo "=== git-fs stderr (last 10 lines) ==="
	tail -10 "$GITFS_LOG"
	echo ""
	echo "=== git-fs trajectory (last 30 monitor samples) ==="
	if [ -n "$STATE_LOG" ] && [ -f "$STATE_LOG" ]; then
		tail -30 "$STATE_LOG"
	else
		echo "(no state log)"
	fi
	echo ""
	echo "=== recent dmesg (fuse/oom/git-fs) ==="
	dmesg --time-format=iso 2>/dev/null \
		| grep -iE 'fuse|oom|killed|git-fs' \
		| tail -10
	if [ $? -ne 0 ]; then
		echo "(dmesg unavailable; for next run: sudo sh -c 'echo 0 > /proc/sys/kernel/dmesg_restrict')"
	fi
	echo ""
	echo "=== mount root listing ==="
	ls -la "$MNT" 2>&1 | head -8
}

echo "test_stress:"
echo "  config: WORKERS=$WORKERS ROUNDS=$ROUNDS DURATION=${DURATION:-unset} CHURN=$CHURN RANDOM_SAMPLE=$RANDOM_SAMPLE"

if [ -n "$EXT_REPO" ]; then
	REPO="$EXT_REPO"
	echo "  repo: $REPO (external)"
else
	echo "  repo: creating test repo..."
	setup_repo
	echo "  repo: $REPO"
fi

ALL_COMMITS="$WORK_DIR/commits-all.txt"
git -C "$REPO" rev-list --all > "$ALL_COMMITS"
total_commits=$(wc -l < "$ALL_COMMITS")
echo "  commits: $total_commits total"

if [ "$total_commits" -eq 0 ]; then
	echo "error: no commits found in repo" >&2
	exit 1
fi

if [ "$CHURN" = "1" ]; then
	if ! git -C "$REPO" update-ref refs/heads/stress-churn-probe HEAD 2>/dev/null \
		|| ! git -C "$REPO" update-ref -d refs/heads/stress-churn-probe 2>/dev/null; then
		echo "error: CHURN=1 but cannot write refs in $REPO" >&2
		exit 1
	fi
	churn_setup
	echo "  churn: enabled (interval=${CHURN_INTERVAL}s, head_backup=${CHURN_HEAD_BACKUP})"
fi

sample_commits() {
	if [ "$RANDOM_SAMPLE" = "1" ] && command -v shuf > /dev/null 2>&1; then
		shuf -n "$MAX_COMMITS" "$ALL_COMMITS" > "$WORK_DIR/commits.txt"
	else
		head -"$MAX_COMMITS" "$ALL_COMMITS" > "$WORK_DIR/commits.txt"
	fi
}
sample_commits

mount_gitfs "$REPO" "$MNT"
echo "  mount: $MNT (pid $GITFS_PID)"

STATE_LOG="$WORK_DIR/state.log"
run_monitor &
monitor_pid=$!

T0=$(date +%s)
round=1
while [ $round -le "$ROUNDS" ]; do
	now=$(date +%s)
	elapsed=$((now - T0))
	if [ -n "$DURATION_SECS" ] && [ "$elapsed" -ge "$DURATION_SECS" ]; then
		break
	fi
	if [ -n "$DURATION_SECS" ]; then
		printf "  [T+%ds] round %d (cap %ds, ROUNDS=%d): " \
			"$elapsed" "$round" "$DURATION_SECS" "$ROUNDS"
	else
		printf "  round %d/%d: " "$round" "$ROUNDS"
	fi

	[ "$RANDOM_SAMPLE" = "1" ] && sample_commits

	run_workers "$round"

	if [ -s "$ERR_LOG" ]; then
		echo "FAILED"
		print_gitfs_diagnostics
		echo
		echo "=== FAILURES ==="
		head -80 "$ERR_LOG"
		remaining=$(wc -l < "$ERR_LOG")
		if [ "$remaining" -gt 80 ]; then
			echo "... and $((remaining - 80)) more"
		fi
		exit 1
	fi

	counts=$(aggregate)
	pass=$(echo "$counts" | cut -d' ' -f1)
	printf "%d checks passed\n" "$pass"

	round=$((round + 1))
done

if [ -n "$OWN_REPO" ]; then
	echo "  mutation phase:"
	run_mutation_phase

	if [ -s "$ERR_LOG" ]; then
		echo "  mutation: FAILED"
		print_gitfs_diagnostics
		echo
		echo "=== FAILURES ==="
		head -80 "$ERR_LOG"
		exit 1
	fi
	echo "  mutation: passed"
fi

print_resource_summary
counts=$(aggregate)
pass=$(echo "$counts" | cut -d' ' -f1)
fail=$(echo "$counts" | cut -d' ' -f2)
total=$((pass + fail))

echo
if [ "$total" -eq 0 ]; then
	echo "error: no integrity checks were executed" >&2
	exit 1
fi
rounds_run=$((round - 1))
[ $rounds_run -lt 1 ] && rounds_run=1
# at least 10 checks per worker and round
min_expected=$((WORKERS * rounds_run * 10))
if [ "$total" -lt "$min_expected" ]; then
	echo "error: only $total checks ran, expected >= $min_expected" >&2
	exit 1
fi
echo "$pass/$total integrity checks passed ($WORKERS workers, $rounds_run rounds)"
[ "$fail" -eq 0 ]
