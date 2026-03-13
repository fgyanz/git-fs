#!/bin/sh
# Stress test for git-fs data integrity
# Usage: ./tests/test_stress.sh [/path/to/git-fs-binary]
#
# Validates that every byte read through the FUSE mount matches the
# actual git object data, under heavy concurrent load.
#
# Environment variables:
#   WORKERS     - concurrent reader processes (default: 16)
#   ROUNDS      - repeat full verification N times (default: 5)
#   REPO        - external repo path; empty = create test repo
#   MAX_COMMITS - cap commits when using external repo (default: 20)

set -e

GITFS="${1:-./git-fs}"
WORKERS="${WORKERS:-16}"
ROUNDS="${ROUNDS:-5}"
EXT_REPO="${REPO:-}"
MAX_COMMITS="${MAX_COMMITS:-20}"

if [ ! -x "$GITFS" ]; then
	echo "error: $GITFS not found or not executable" >&2
	exit 1
fi

TMPBASE="${TMPDIR:-/tmp}"
MNT=$(mktemp -d "$TMPBASE/git-fs-stress-mnt-XXXXXX")
ERR_LOG=$(mktemp "$TMPBASE/git-fs-stress-err-XXXXXX")
WORK_DIR=$(mktemp -d "$TMPBASE/git-fs-stress-work-XXXXXX")
OWN_REPO=""

cleanup() {
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
	fls=$(ls -1a "$1" 2>/dev/null | grep -v '^\.\.\{0,1\}$' | sort)
	gls=$(git -C "$REPO" ls-tree --name-only "$2" "$3/" | sed "s|^$3/||" | sort)
	if [ "$fls" != "$gls" ]; then
		ftmp2=$(mktemp "$WORK_DIR/fls-XXXXXX")
		gtmp2=$(mktemp "$WORK_DIR/gls-XXXXXX")
		echo "$fls" > "$ftmp2"
		echo "$gls" > "$gtmp2"
		echo "FAIL readdir $3 @ $2 (fuse=$(wc -l < "$ftmp2") git=$(wc -l < "$gtmp2"))" >> "$ERR_LOG"
		diff "$gtmp2" "$ftmp2" | head -5 >> "$ERR_LOG"
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
# Derives expected SHA from git independently — never trusts FUSE output.
worker_parent_chain() {
	set +e
	wid=$1; round=$2
	pass=0; fail=0
	cfile_p="$WORK_DIR/pass-D-$round-$wid"
	cfile_f="$WORK_DIR/fail-D-$round-$wid"

	# build the expected parent chain from git directly
	chaintmp="$WORK_DIR/chain-D-$round-$wid.tmp"
	sha=$(git -C "$REPO" rev-parse HEAD)
	depth=0
	while [ $depth -lt 25 ] && [ -n "$sha" ]; do
		echo "$sha" >> "$chaintmp"
		sha=$(git -C "$REPO" log -1 --format='%P' "$sha" | cut -d' ' -f1)
		depth=$((depth + 1))
	done

	# now walk the FUSE parent chain and verify against the git chain
	expected_depth=$(wc -l < "$chaintmp")
	cpath="$MNT/HEAD"
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
worker_refs() {
	set +e
	wid=$1; round=$2
	pass=0; fail=0
	cfile_p="$WORK_DIR/pass-E-$round-$wid"
	cfile_f="$WORK_DIR/fail-E-$round-$wid"

	# branches — dump to file to avoid SIGPIPE issues with | head
	brtmp="$WORK_DIR/br-E-$round-$wid.tmp"
	git -C "$REPO" for-each-ref --format='%(refname:short)' refs/heads/ --count=10 > "$brtmp"
	while IFS= read -r br; do
		sha=$(git -C "$REPO" rev-parse "$br")
		brname=$(echo "$br" | sed 's|.*/||')
		bpath="$MNT/branches/heads/$brname"
		if [ ! -d "$bpath" ]; then
			echo "FAIL branch $brname @ $sha: FUSE path missing $bpath" >> "$ERR_LOG"
			fail=$((fail + 1))
			continue
		fi
		count "$(verify_hash "$bpath/hash" "$sha")"
		count "$(verify_msg "$bpath/msg" "$sha")"
		# verify one blob
		sample=$(git -C "$REPO" ls-tree -r --name-only "$sha" | sed '1q')
		if [ -n "$sample" ] && [ -f "$bpath/tree/$sample" ]; then
			count "$(verify_blob "$bpath/tree/$sample" "$sha" "$sample")"
		fi
	done < "$brtmp"
	rm -f "$brtmp"

	# tags — dump to file to avoid SIGPIPE
	tagtmp="$WORK_DIR/tag-E-$round-$wid.tmp"
	git -C "$REPO" tag -l > "$tagtmp"
	ntags=0
	while IFS= read -r tag && [ $ntags -lt 10 ]; do
		sha=$(git -C "$REPO" rev-parse "$tag^{commit}" 2>/dev/null) || continue
		tpath="$MNT/tags/$tag"
		if [ ! -d "$tpath" ]; then
			echo "FAIL tag $tag @ $sha: FUSE path missing $tpath" >> "$ERR_LOG"
			fail=$((fail + 1))
			continue
		fi
		count "$(verify_hash "$tpath/hash" "$sha")"
		ntags=$((ntags + 1))
	done < "$tagtmp"
	rm -f "$tagtmp"

	write_counts
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
	# A: 25%, B: 25%, C: 25%, D: 12.5%, E: 12.5%
	wa=$((WORKERS / 4))
	wb=$((WORKERS / 4))
	wc_n=$((WORKERS / 4))
	wd=$((WORKERS / 8))
	we=$((WORKERS - wa - wb - wc_n - wd))
	# ensure at least 1 per scenario
	[ $wa -lt 1 ] && wa=1
	[ $wb -lt 1 ] && wb=1
	[ $wc_n -lt 1 ] && wc_n=1
	[ $wd -lt 1 ] && wd=1
	[ $we -lt 1 ] && we=1

	wid=0

	# Scenario A: full tree walk
	i=0
	while [ $i -lt $wa ]; do
		line=$(( (wid % num_commits) + 1 ))
		sha=$(sed -n "${line}p" "$WORK_DIR/commits.txt")
		worker_tree_walk "$wid" "$sha" "$round" &
		wid=$((wid + 1))
		i=$((i + 1))
	done

	# Scenario B: same-file contention
	i=0
	while [ $i -lt $wb ]; do
		worker_contention "$wid" "$head_sha" "$round" &
		wid=$((wid + 1))
		i=$((i + 1))
	done

	# Scenario C: readdir + read interleave
	i=0
	while [ $i -lt $wc_n ]; do
		worker_readdir_read "$wid" "$round" &
		wid=$((wid + 1))
		i=$((i + 1))
	done

	# Scenario D: parent chain walk
	i=0
	while [ $i -lt $wd ]; do
		worker_parent_chain "$wid" "$round" &
		wid=$((wid + 1))
		i=$((i + 1))
	done

	# Scenario E: branch/tag cross-walk
	i=0
	while [ $i -lt $we ]; do
		worker_refs "$wid" "$round" &
		wid=$((wid + 1))
		i=$((i + 1))
	done

	wait
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

# ── Main ────────────────────────────────────────────────────────────

echo "test_stress:"
echo "  config: WORKERS=$WORKERS ROUNDS=$ROUNDS"

# setup
if [ -n "$EXT_REPO" ]; then
	REPO="$EXT_REPO"
	echo "  repo: $REPO (external)"
else
	echo "  repo: creating test repo..."
	setup_repo
	echo "  repo: $REPO"
fi

# build commit list
git -C "$REPO" rev-list --all | head -"$MAX_COMMITS" > "$WORK_DIR/commits.txt"
num_commits=$(wc -l < "$WORK_DIR/commits.txt")
echo "  commits: $num_commits"

if [ "$num_commits" -eq 0 ]; then
	echo "error: no commits found in repo" >&2
	exit 1
fi

# mount
"$GITFS" -r "$REPO" -m "$MNT"

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
round=1
while [ $round -le "$ROUNDS" ]; do
	printf "  round %d/%d: " "$round" "$ROUNDS"
	run_workers "$round"

	# check for failures
	if [ -s "$ERR_LOG" ]; then
		echo "FAILED"
		echo
		echo "=== FAILURES ==="
		head -50 "$ERR_LOG"
		remaining=$(wc -l < "$ERR_LOG")
		if [ "$remaining" -gt 50 ]; then
			echo "... and $((remaining - 50)) more"
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
		echo
		echo "=== FAILURES ==="
		head -50 "$ERR_LOG"
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
# sanity: expect at least 10 checks per worker per round (blobs + hashes + msgs)
min_expected=$((WORKERS * ROUNDS * 10))
if [ "$total" -lt "$min_expected" ]; then
	echo "error: only $total checks ran, expected >= $min_expected" >&2
	exit 1
fi
echo "$pass/$total integrity checks passed ($WORKERS workers, $ROUNDS rounds)"
[ "$fail" -eq 0 ]
