#!/bin/sh
# Stress test for git-fs data integrity
# Usage: ./tests/test_stress.sh [/path/to/git-fs-binary]
#
# Validates that every byte read through the FUSE mount matches the
# actual git object data, under heavy concurrent load.
#
# Environment variables:
#   WORKERS         - concurrent reader processes (default: 16)
#   ROUNDS          - hard cap on rounds (default: 5)
#   REPO            - external repo path; empty = create test repo
#   MAX_COMMITS     - commits sampled from the repo (default: 20)
#   DURATION        - wall-clock cap, e.g. "5m", "2h", "1h30m", "300s".
#                     When set, rounds keep running until elapsed time
#                     exceeds DURATION; ROUNDS becomes a hard upper cap.
#   RANDOM_SAMPLE   - 1 = re-sample commits + refs at random per round.
#                     Default: 1 if DURATION is set, else 0.
#   CHURN           - 1 = run a ref-churning background loop alongside
#                     readers. Mutates only refs/heads/stress-churn/*,
#                     refs/tags/stress-churn-*, and HEAD. Default: 0.
#                     Requires REPO to be writable.
#   CHURN_INTERVAL  - sleep between churn ops (default: 0.1, seconds).

set -e

GITFS="${1:-./git-fs}"
WORKERS="${WORKERS:-16}"
ROUNDS="${ROUNDS:-5}"
EXT_REPO="${REPO:-}"
MAX_COMMITS="${MAX_COMMITS:-20}"
DURATION="${DURATION:-}"
CHURN="${CHURN:-0}"
CHURN_INTERVAL="${CHURN_INTERVAL:-0.1}"
# default RANDOM_SAMPLE to 1 when DURATION is set, else 0
if [ -z "${RANDOM_SAMPLE:-}" ]; then
	if [ -n "$DURATION" ]; then RANDOM_SAMPLE=1; else RANDOM_SAMPLE=0; fi
fi

# parse a duration like "5m", "2h30m", "300s", "1h" → seconds
# echoes the parsed seconds, or echoes nothing on parse failure
duration_to_seconds() {
	d=$1
	[ -z "$d" ] && return 0
	# pure digits → seconds
	case "$d" in
		*[!0-9]*) ;;
		*) echo "$d"; return 0 ;;
	esac
	# parse h, m, s components
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

if [ ! -x "$GITFS" ]; then
	echo "error: $GITFS not found or not executable" >&2
	exit 1
fi

# Resolve to an absolute path: the mount step cds into the repo.
GITFS=$(readlink -f "$GITFS")

TMPBASE="${TMPDIR:-/tmp}"
MNT=$(mktemp -d "$TMPBASE/git-fs-stress-mnt-XXXXXX")
ERR_LOG=$(mktemp "$TMPBASE/git-fs-stress-err-XXXXXX")
WORK_DIR=$(mktemp -d "$TMPBASE/git-fs-stress-work-XXXXXX")
OWN_REPO=""

cleanup() {
	# stop and clean the churner first — needs REPO to still be there
	if [ "$CHURN" = "1" ]; then
		: > "$WORK_DIR/churner-stop" 2>/dev/null || true
		churn_cleanup 2>/dev/null || true
	fi
	fusermount3 -u "$MNT" 2>/dev/null || true
	rm -rf "$MNT" "$ERR_LOG" "$WORK_DIR"
	[ -n "$OWN_REPO" ] && rm -rf "$OWN_REPO"
}
trap cleanup EXIT

# ── Verification functions ──────────────────────────────────────────
# These never return non-zero — they log failures and echo 0/1.
# Callers capture the echoed value to count pass/fail.

verify_blob() {
	# $1 = fuse path, $2 = commit SHA, $3 = repo-relative path
	# Read both FUSE and git into temp files for atomic hash+size comparison.
	# No pipelines — exit codes are checked explicitly.
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
	# $1 = fuse path, $2 = commit SHA, $3 = repo-relative path
	# git stores symlinks as blobs containing the target path.
	# FUSE may expose them as symlinks (readlink) or as regular files (cat).
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
	# $1 = fuse path to hash file, $2 = expected SHA (from git, not FUSE)
	got=$(cat "$1" 2>/dev/null || echo "READ_ERROR")
	got=$(printf '%s' "$got" | tr -d '\n')
	if [ "$got" != "$2" ]; then
		echo "FAIL hash: fuse=$got expected=$2 path=$1" >> "$ERR_LOG"
		echo 0; return 0
	fi
	echo 1; return 0
}

verify_msg() {
	# $1 = fuse path to msg file, $2 = commit SHA
	fmsg=$(cat "$1" 2>/dev/null || echo "READ_ERROR")
	gmsg=$(git -C "$REPO" log -1 --format='%B' "$2")
	# guard against empty-vs-empty: commit messages are always non-empty,
	# but verify explicitly
	if [ -z "$gmsg" ] && [ -z "$fmsg" ]; then
		# both empty — cannot distinguish a FUSE bug from a truly empty msg
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
	# $1 = fuse dir path, $2 = commit SHA, $3 = repo-relative dir
	# ls -1a to include dotfiles; grep -v to strip . and ..
	# capture the ls exit status separately so we can distinguish
	# "ls failed (EIO/ENOENT)" from "directory listed but is empty".
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
		# count actual entries (zero when empty, no off-by-one from
		# `echo`'s implicit trailing newline)
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

# ── Worker helpers ──────────────────────────────────────────────────

count() {
	# $1 = verify result (0 or 1), uses $pass $fail from caller scope
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

# ── Worker functions ────────────────────────────────────────────────
# Workers disable set -e so verify failures don't abort the subshell.

# Scenario A: full tree walk — verify every blob in a commit
worker_tree_walk() {
	set +e
	wid=$1; sha=$2; round=$3
	pass=0; fail=0
	cfile_p="$WORK_DIR/pass-A-$round-$wid"
	cfile_f="$WORK_DIR/fail-A-$round-$wid"

	# dump file list with modes to handle symlinks (120000) separately
	lstmp="$WORK_DIR/ls-A-$round-$wid.tmp"
	git -C "$REPO" ls-tree -r "$sha" > "$lstmp"
	while IFS='	' read -r meta path; do
		mode=$(echo "$meta" | cut -d' ' -f1)
		fpath="$MNT/objects/$sha/tree/$path"
		if [ "$mode" = "120000" ]; then
			# symlink: verify target string matches git blob content
			count "$(verify_symlink "$fpath" "$sha" "$path")"
		elif [ "$mode" = "160000" ]; then
			# submodule: skip (not a blob)
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

# Scenario B: same-file contention — multiple workers read same files
# Uses immutable objects/ path so HEAD mutation can't cause stale SHA mismatch.
worker_contention() {
	set +e
	wid=$1; head_sha=$2; round=$3
	pass=0; fail=0
	cfile_p="$WORK_DIR/pass-B-$round-$wid"
	cfile_f="$WORK_DIR/fail-B-$round-$wid"

	# pick two files: largest blob + first text file
	big=$(git -C "$REPO" ls-tree -r -l "$head_sha" | sort -k4 -rn | sed '1q' | awk '{print $5}')
	small=$(git -C "$REPO" ls-tree -r --name-only "$head_sha" | sed '1q')

	# use immutable objects/$sha path, not HEAD (which is mutable)
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

# Scenario C: readdir + read interleave
worker_readdir_read() {
	set +e
	wid=$1; round=$2
	pass=0; fail=0
	cfile_p="$WORK_DIR/pass-C-$round-$wid"
	cfile_f="$WORK_DIR/fail-C-$round-$wid"

	while IFS= read -r sha; do
		# find a subtree directory in this commit
		dir=$(git -C "$REPO" ls-tree "$sha" | awk '$2=="tree"{print $4; exit}')
		[ -z "$dir" ] && continue
		[ -d "$MNT/objects/$sha/tree/$dir" ] || continue

		count "$(verify_tree_listing "$MNT/objects/$sha/tree/$dir" "$sha" "$dir")"

		# read up to 20 files from the listing
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

# Scenario D: parent chain walk
# Captures FUSE's HEAD once, then walks the rest via immutable
# objects/$sha/parent paths. Under churn the HEAD can move, but the
# captured SHA still names a commit reachable in git history (or did
# at the moment of capture); we verify exactly that.
worker_parent_chain() {
	set +e
	wid=$1; round=$2
	pass=0; fail=0
	cfile_p="$WORK_DIR/pass-D-$round-$wid"
	cfile_f="$WORK_DIR/fail-D-$round-$wid"

	# capture FUSE's view of HEAD. strip trailing newline.
	head_sha=$(cat "$MNT/HEAD/hash" 2>/dev/null | tr -d '\n')
	if [ -z "$head_sha" ]; then
		echo "FAIL parent chain: cannot read MNT/HEAD/hash" >> "$ERR_LOG"
		count 0; write_counts; return
	fi
	# the captured SHA must name a real commit (FUSE never invents SHAs)
	if ! git -C "$REPO" cat-file -e "${head_sha}^{commit}" 2>/dev/null; then
		echo "FAIL parent chain: FUSE HEAD '$head_sha' is not a commit" >> "$ERR_LOG"
		count 0; write_counts; return
	fi
	# verify the msg via the immutable objects/ path keyed by the
	# captured SHA. reading MNT/HEAD/msg here would race with the
	# churner: HEAD can move between the hash read above and the msg
	# read, so MNT/HEAD/msg may already point at a different commit.
	count "$(verify_msg "$MNT/objects/$head_sha/msg" "$head_sha")"

	# build expected parent chain from the captured SHA (immutable)
	chaintmp="$WORK_DIR/chain-D-$round-$wid.tmp"
	sha=$head_sha
	depth=0
	while [ $depth -lt 25 ] && [ -n "$sha" ]; do
		echo "$sha" >> "$chaintmp"
		sha=$(git -C "$REPO" log -1 --format='%P' "$sha" | cut -d' ' -f1)
		depth=$((depth + 1))
	done

	# walk the immutable parent chain rooted at the captured HEAD SHA
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

	# detect truncated chain: FUSE missing parent/ dirs for commits that have parents
	if [ "$depth" -lt "$expected_depth" ]; then
		echo "FAIL parent chain: walked $depth of $expected_depth commits" >> "$ERR_LOG"
		count 0
	fi
	write_counts
}

# Scenario E: branch/tag cross-walk
# Captures FUSE's view of each ref first, then verifies that view is
# self-consistent with some valid git state. Under churn the ref may
# move between reads — that's fine, we don't assert "FUSE matches git
# this instant", we assert "FUSE returned a SHA that is a real commit
# and the data hanging off that ref is the data git stores under the
# captured SHA". Tree reads go through immutable objects/$sha/tree to
# decouple from further ref motion mid-walk.
worker_refs() {
	set +e
	wid=$1; round=$2
	pass=0; fail=0
	cfile_p="$WORK_DIR/pass-E-$round-$wid"
	cfile_f="$WORK_DIR/fail-E-$round-$wid"

	# branches — sample N at random when RANDOM_SAMPLE is on, head -N otherwise
	brtmp="$WORK_DIR/br-E-$round-$wid.tmp"
	if [ "$RANDOM_SAMPLE" = "1" ] && command -v shuf > /dev/null 2>&1; then
		git -C "$REPO" for-each-ref --format='%(refname:short)' refs/heads/ \
			| shuf -n 10 > "$brtmp"
	else
		git -C "$REPO" for-each-ref --format='%(refname:short)' refs/heads/ \
			--count=10 > "$brtmp"
	fi
	while IFS= read -r br; do
		# skip our own churn refs — they're created/deleted under us
		case "$br" in stress-churn/*) continue ;; esac
		brname=$(echo "$br" | sed 's|.*/||')
		bpath="$MNT/branches/heads/$brname"
		if [ ! -d "$bpath" ]; then
			# under churn a branch can be deleted between for-each-ref
			# and our visit; tolerate that
			continue
		fi
		# capture FUSE's view of this branch
		captured=$(cat "$bpath/hash" 2>/dev/null | tr -d '\n')
		if [ -z "$captured" ]; then
			echo "FAIL branch $brname: cannot read $bpath/hash" >> "$ERR_LOG"
			count 0; continue
		fi
		# the captured SHA must be a real commit
		if ! git -C "$REPO" cat-file -e "${captured}^{commit}" 2>/dev/null; then
			echo "FAIL branch $brname: FUSE hash '$captured' is not a commit" >> "$ERR_LOG"
			count 0; continue
		fi
		# verify msg via the immutable objects/ path — reading
		# $bpath/msg would race with churner ref moves
		count "$(verify_msg "$MNT/objects/$captured/msg" "$captured")"
		# verify a blob via the immutable objects/ path keyed by the captured SHA
		sample=$(git -C "$REPO" ls-tree -r --name-only "$captured" 2>/dev/null | sed '1q')
		if [ -n "$sample" ] && [ -f "$MNT/objects/$captured/tree/$sample" ]; then
			count "$(verify_blob "$MNT/objects/$captured/tree/$sample" "$captured" "$sample")"
		fi
	done < "$brtmp"
	rm -f "$brtmp"

	# tags — random sample when enabled
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
			# tolerate deletion under us
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
		# FUSE returned a self-consistent valid commit SHA for the tag
		count 1
	done < "$tagtmp"
	rm -f "$tagtmp"

	write_counts
}

# Scenario F: same-fh shared-descriptor stress
# Independent-fd parallel reads (scenarios A/B) catch most corruption
# bugs, but a shared-fd path (multiple kernel readers consuming a single
# open file description) exercises per-gitfs_fh state in a way that
# independent fds don't. Open a moderately-sized blob once via exec 3<,
# then spawn N background cats reading from <&3. The kernel serializes
# their reads on the shared file position, so each gets a contiguous
# slice and the union covers the file exactly. We verify the sum of
# slice sizes equals the file size — torn or duplicated bytes from a
# broken concurrent-read path would change that sum.
worker_same_fh() {
	set +e
	wid=$1; sha=$2; round=$3
	pass=0; fail=0
	cfile_p="$WORK_DIR/pass-F-$round-$wid"
	cfile_f="$WORK_DIR/fail-F-$round-$wid"

	# pick a blob between 64K and 1M — large enough to span multiple
	# FUSE_READ requests, small enough to be cheap.
	path=$(git -C "$REPO" ls-tree -r -l "$sha" 2>/dev/null \
		| awk '$4 > 65536 && $4 < 1048576 {print substr($0, index($0, $5)); exit}')
	# fallback: any non-empty blob if no medium one exists
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

		# fd 3 is shared across the three forked cats; the kernel
		# arbitrates the position, each gets a disjoint slice.
		exec 3< "$fpath"
		cat <&3 > "$out1" &
		cat <&3 > "$out2" &
		cat <&3 > "$out3" &
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

# ── Ref churner ─────────────────────────────────────────────────────
# When CHURN=1, this loop runs alongside the readers and continuously
# mutates refs in a private namespace. Exercises the inotify-driven
# invalidation path under sustained load. All mutations are confined
# to refs/heads/stress-churn/* and refs/tags/stress-churn-*; HEAD is
# moved transiently and restored on cleanup. Reads against these
# private refs are filtered out in scenario E (see worker_refs).

CHURN_HEAD_BACKUP=""

churn_setup() {
	CHURN_HEAD_BACKUP=$(git -C "$REPO" rev-parse HEAD 2>/dev/null)
}

churn_cleanup() {
	[ -z "$CHURN_HEAD_BACKUP" ] && return
	# restore HEAD
	git -C "$REPO" update-ref HEAD "$CHURN_HEAD_BACKUP" 2>/dev/null || true
	# delete every stress-churn ref (loose + packed)
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

# ── Repo setup ──────────────────────────────────────────────────────

setup_repo() {
	REPO=$(mktemp -d "$TMPBASE/git-fs-stress-repo-XXXXXX")
	OWN_REPO="$REPO"

	git -C "$REPO" init -q
	git -C "$REPO" config user.email "stress@test"
	git -C "$REPO" config user.name "stress"

	# commit 1: deep tree + wide dir + binary files
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
	# empty blob
	: > "$REPO/empty.txt"
	# symlink
	(cd "$REPO" && ln -s a/b/c/d/e/f.txt link-to-f.txt)
	git -C "$REPO" add .
	git -C "$REPO" commit -q -m "commit 1: initial structure"
	git -C "$REPO" tag v0.1

	# commits 2-20: modify files, add new ones
	i=2
	while [ $i -le 20 ]; do
		echo "revision $i" > "$REPO/a/b/c/d/e/f.txt"
		echo "new-file-$i" > "$REPO/file-$i.txt"
		git -C "$REPO" add .
		git -C "$REPO" commit -q -m "commit $i"
		case $i in
			5)  git -C "$REPO" branch br-1; git -C "$REPO" tag v0.2 ;;
			10) git -C "$REPO" branch br-2; git -C "$REPO" tag v0.3 ;;
			15) git -C "$REPO" branch br-3; git -C "$REPO" tag v0.4 ;;
			20) git -C "$REPO" branch br-4 ;;
		esac
		i=$((i + 1))
	done
}

# ── Distribute workers across scenarios ─────────────────────────────

run_workers() {
	round=$1
	head_sha=$(git -C "$REPO" rev-parse HEAD)
	num_commits=$(wc -l < "$WORK_DIR/commits.txt")

	if [ "$num_commits" -eq 0 ]; then
		echo "error: no commits found in repo" >&2
		exit 1
	fi

	# assign workers to scenarios
	# A: 22%, B: 22%, C: 22%, D: 11%, E: 11%, F: 11% (~10%)
	wa=$((WORKERS * 22 / 100))
	wb=$((WORKERS * 22 / 100))
	wc_n=$((WORKERS * 22 / 100))
	wd=$((WORKERS / 10))
	we=$((WORKERS / 10))
	wf=$((WORKERS - wa - wb - wc_n - wd - we))
	# ensure at least 1 per scenario
	[ $wa -lt 1 ] && wa=1
	[ $wb -lt 1 ] && wb=1
	[ $wc_n -lt 1 ] && wc_n=1
	[ $wd -lt 1 ] && wd=1
	[ $we -lt 1 ] && we=1
	[ $wf -lt 1 ] && wf=1

	wid=0
	reader_pids=""

	# start the churner alongside the read workers when CHURN=1.
	# the churner runs in its own pid, separate from the readers, so
	# we can wait on readers without waiting on the churner.
	if [ "$CHURN" = "1" ]; then
		run_churner "$round" &
		churner_pid=$!
	fi

	# Scenario A: full tree walk
	i=0
	while [ $i -lt $wa ]; do
		line=$(( (wid % num_commits) + 1 ))
		sha=$(sed -n "${line}p" "$WORK_DIR/commits.txt")
		worker_tree_walk "$wid" "$sha" "$round" &
		reader_pids="$reader_pids $!"
		wid=$((wid + 1))
		i=$((i + 1))
	done

	# Scenario B: same-file contention
	i=0
	while [ $i -lt $wb ]; do
		worker_contention "$wid" "$head_sha" "$round" &
		reader_pids="$reader_pids $!"
		wid=$((wid + 1))
		i=$((i + 1))
	done

	# Scenario C: readdir + read interleave
	i=0
	while [ $i -lt $wc_n ]; do
		worker_readdir_read "$wid" "$round" &
		reader_pids="$reader_pids $!"
		wid=$((wid + 1))
		i=$((i + 1))
	done

	# Scenario D: parent chain walk
	i=0
	while [ $i -lt $wd ]; do
		worker_parent_chain "$wid" "$round" &
		reader_pids="$reader_pids $!"
		wid=$((wid + 1))
		i=$((i + 1))
	done

	# Scenario E: branch/tag cross-walk
	i=0
	while [ $i -lt $we ]; do
		worker_refs "$wid" "$round" &
		reader_pids="$reader_pids $!"
		wid=$((wid + 1))
		i=$((i + 1))
	done

	# Scenario F: same-fh shared-descriptor stress
	i=0
	while [ $i -lt $wf ]; do
		line=$(( (wid % num_commits) + 1 ))
		sha=$(sed -n "${line}p" "$WORK_DIR/commits.txt")
		worker_same_fh "$wid" "$sha" "$round" &
		reader_pids="$reader_pids $!"
		wid=$((wid + 1))
		i=$((i + 1))
	done

	# wait only on read workers (bare `wait` would also block on the
	# churner, which doesn't exit until we signal it)
	for pid in $reader_pids; do
		wait "$pid" 2>/dev/null
	done

	if [ "$CHURN" = "1" ]; then
		: > "$WORK_DIR/churner-stop"
		wait "$churner_pid" 2>/dev/null
		rm -f "$WORK_DIR/churner-stop"
	fi
}

# ── Ref mutation phase ──────────────────────────────────────────────

run_mutation_phase() {
	echo "  mutation: starting background readers..."
	mut_pass=0
	mut_fail=0

	# start background readers on immutable object paths
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

	# mutate the repo: add 5 commits + 1 branch
	i=1
	while [ $i -le 5 ]; do
		echo "mutation $i" > "$REPO/mutated-$i.txt"
		git -C "$REPO" add "mutated-$i.txt"
		git -C "$REPO" commit -q -m "mutation commit $i"
		i=$((i + 1))
	done
	git -C "$REPO" branch mutation-branch

	# wait for ref cache to expire
	sleep 2

	# verify HEAD updated
	new_head=$(cat "$MNT/HEAD/hash")
	new_head=$(printf '%s' "$new_head" | tr -d '\n')
	actual_head=$(git -C "$REPO" rev-parse HEAD)
	if [ "$new_head" != "$actual_head" ]; then
		echo "FAIL mutation: HEAD not updated: fuse=$new_head git=$actual_head" >> "$ERR_LOG"
		mut_fail=$((mut_fail + 1))
	else
		mut_pass=$((mut_pass + 1))
	fi

	# verify new branch visible
	if ls "$MNT/branches/heads/" | grep -q "mutation-branch"; then
		mut_pass=$((mut_pass + 1))
	else
		echo "FAIL mutation: mutation-branch not visible" >> "$ERR_LOG"
		mut_fail=$((mut_fail + 1))
	fi

	# verify new file readable
	r=$(verify_blob "$MNT/HEAD/tree/mutated-5.txt" "$actual_head" "mutated-5.txt")
	if [ "$r" = "1" ]; then
		mut_pass=$((mut_pass + 1))
	else
		mut_fail=$((mut_fail + 1))
	fi

	# write mutation counts
	echo "$mut_pass" > "$WORK_DIR/pass-mut-checks"
	echo "$mut_fail" > "$WORK_DIR/fail-mut-checks"

	# wait for background readers
	for pid in $bg_pids; do
		wait "$pid" 2>/dev/null || true
	done
}

# ── Aggregate results ───────────────────────────────────────────────

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

# Background monitor: samples git-fs's /proc/<pid>/status every
# MONITOR_INTERVAL seconds and appends a one-line summary to a state
# log. When git-fs disappears, the monitor records "DIED" with the
# elapsed time and exits. On test failure, print the tail of this log
# so we see RSS / Threads / FDSize trajectory leading up to the crash.
#
# Polling has an inherent miss window: by the time we detect the
# process is gone, /proc/<pid> is unreadable, so the LAST captured
# sample is our last view of git-fs alive. A 2s interval gives us a
# reasonable trajectory without flooding the log.

MONITOR_INTERVAL="${MONITOR_INTERVAL:-2}"
STATE_LOG=""
monitor_pid=""

run_monitor() {
	# wait briefly for git-fs to start
	t=0
	pid=""
	while [ $t -lt 50 ] && [ -z "$pid" ]; do
		pid=$(pgrep -f "git-fs.*$MNT" 2>/dev/null | head -1)
		[ -z "$pid" ] && pid=$(pgrep -x git-fs 2>/dev/null | head -1)
		[ -n "$pid" ] && break
		sleep 0.1
		t=$((t + 1))
	done
	if [ -z "$pid" ]; then
		echo "monitor: never found git-fs pid" >> "$STATE_LOG"
		return
	fi
	echo "monitor: tracking pid=$pid (interval=${MONITOR_INTERVAL}s)" >> "$STATE_LOG"
	t0=$(date +%s)
	while [ ! -f "$WORK_DIR/monitor-stop" ]; do
		if [ ! -d "/proc/$pid" ]; then
			now=$(date +%s)
			echo "[T+$((now - t0))s] DIED — /proc/$pid gone" >> "$STATE_LOG"
			return
		fi
		# selected fields, formatted compact for tailability
		now=$(date +%s)
		state=$(awk '/^State:/{print $2}' "/proc/$pid/status" 2>/dev/null)
		threads=$(awk '/^Threads:/{print $2}' "/proc/$pid/status" 2>/dev/null)
		rss=$(awk '/^VmRSS:/{print $2}' "/proc/$pid/status" 2>/dev/null)
		hwm=$(awk '/^VmHWM:/{print $2}' "/proc/$pid/status" 2>/dev/null)
		size=$(awk '/^VmSize:/{print $2}' "/proc/$pid/status" 2>/dev/null)
		fds=$(awk '/^FDSize:/{print $2}' "/proc/$pid/status" 2>/dev/null)
		printf '[T+%ds] state=%s threads=%s VmRSS=%sk VmHWM=%sk VmSize=%sk FDSize=%s\n' \
			$((now - t0)) "$state" "$threads" "$rss" "$hwm" "$size" "$fds" \
			>> "$STATE_LOG"
		sleep "$MONITOR_INTERVAL"
	done
}

# Print a final snapshot + the monitor trajectory directly to stdout
# so it survives the head -80 truncation of ERR_LOG.
print_gitfs_diagnostics() {
	echo ""
	echo "=== git-fs process state at time of failure ==="
	pid=$(pgrep -f "git-fs.*$MNT" 2>/dev/null | head -1)
	[ -z "$pid" ] && pid=$(pgrep -x git-fs 2>/dev/null | head -1)
	if [ -z "$pid" ]; then
		echo "(no git-fs pid found — process is gone)"
	else
		echo "pid=$pid"
		grep -E 'State|Threads|VmSize|VmRSS|VmHWM|VmPeak|FDSize|voluntary_ctxt|nonvoluntary' \
			"/proc/$pid/status" 2>/dev/null || echo "(/proc/$pid/status unreadable)"
		echo "--- /proc/$pid/smaps_rollup ---"
		head -8 "/proc/$pid/smaps_rollup" 2>/dev/null || echo "(unreadable)"
	fi
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

# ── Main ────────────────────────────────────────────────────────────

echo "test_stress:"
echo "  config: WORKERS=$WORKERS ROUNDS=$ROUNDS DURATION=${DURATION:-unset} CHURN=$CHURN RANDOM_SAMPLE=$RANDOM_SAMPLE"

# setup
if [ -n "$EXT_REPO" ]; then
	REPO="$EXT_REPO"
	echo "  repo: $REPO (external)"
else
	echo "  repo: creating test repo..."
	setup_repo
	echo "  repo: $REPO"
fi

# full commit list (used as sampling source under RANDOM_SAMPLE)
ALL_COMMITS="$WORK_DIR/commits-all.txt"
git -C "$REPO" rev-list --all > "$ALL_COMMITS"
total_commits=$(wc -l < "$ALL_COMMITS")
echo "  commits: $total_commits total"

if [ "$total_commits" -eq 0 ]; then
	echo "error: no commits found in repo" >&2
	exit 1
fi

# CHURN requires write access to the repo (we update-ref under it).
if [ "$CHURN" = "1" ]; then
	if ! git -C "$REPO" update-ref refs/heads/stress-churn-probe HEAD 2>/dev/null \
		|| ! git -C "$REPO" update-ref -d refs/heads/stress-churn-probe 2>/dev/null; then
		echo "error: CHURN=1 but cannot write refs in $REPO" >&2
		exit 1
	fi
	churn_setup
	echo "  churn: enabled (interval=${CHURN_INTERVAL}s, head_backup=${CHURN_HEAD_BACKUP})"
fi

# (re)build the per-round commit sample. shuf isn't POSIX but is on
# every modern linux; fall back to head if absent.
sample_commits() {
	if [ "$RANDOM_SAMPLE" = "1" ] && command -v shuf > /dev/null 2>&1; then
		shuf -n "$MAX_COMMITS" "$ALL_COMMITS" > "$WORK_DIR/commits.txt"
	else
		head -"$MAX_COMMITS" "$ALL_COMMITS" > "$WORK_DIR/commits.txt"
	fi
}
sample_commits

# mount
(cd "$REPO" && "$GITFS" -m "$MNT")

# poll until mount is ready (up to 5 seconds)
i=0
while [ $i -lt 50 ]; do
	ls "$MNT/HEAD" > /dev/null 2>&1 && break
	sleep 0.1
	i=$((i + 1))
done
if ! ls "$MNT/HEAD" > /dev/null 2>&1; then
	echo "error: mount failed or not ready after 5s" >&2
	exit 1
fi
echo "  mount: $MNT"

# run stress rounds
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

	# resample commits when running random
	[ "$RANDOM_SAMPLE" = "1" ] && sample_commits

	run_workers "$round"

	# check for failures
	if [ -s "$ERR_LOG" ]; then
		echo "FAILED"
		# capture git-fs state before exiting so the dump tells us
		# whether the failure correlates with pool/memory pressure
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

# ref mutation phase (only for own repos — skip for external)
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

# final report
counts=$(aggregate)
pass=$(echo "$counts" | cut -d' ' -f1)
fail=$(echo "$counts" | cut -d' ' -f2)
total=$((pass + fail))

echo
if [ "$total" -eq 0 ]; then
	echo "error: no integrity checks were executed" >&2
	exit 1
fi
# actual rounds run: the round counter is incremented after each
# completed round, so when the loop exits with the time cap or the
# ROUNDS cap, `round - 1` is the last finished round.
rounds_run=$((round - 1))
[ $rounds_run -lt 1 ] && rounds_run=1
# sanity: expect at least 10 checks per worker per round (blobs + hashes + msgs)
min_expected=$((WORKERS * rounds_run * 10))
if [ "$total" -lt "$min_expected" ]; then
	echo "error: only $total checks ran, expected >= $min_expected" >&2
	exit 1
fi
echo "$pass/$total integrity checks passed ($WORKERS workers, $rounds_run rounds)"
[ "$fail" -eq 0 ]
