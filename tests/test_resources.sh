#!/bin/sh
# Resource tests for git-fs: fd usage, memory and leaks.
# Usage: ./tests/test_resources.sh [/path/to/git-fs-binary]
#
# Scenarios:
#   R1  baseline after warm-up
#   R2  open/read/close, partial reads, readdir: fds back to baseline
#   R3  files held open by clients: fds per open file
#   R4  parallel readers: release lag (memfds alive per active reader)
#   R5  git-fs under a low RLIMIT_NOFILE: clients must not see EMFILE
#   R6  same workload repeated: RSS must plateau
#   R7  ref churn (HEAD moves, branches come and go): RSS must plateau
#
# Environment variables:
#   REPO            - external repo path; empty = create test repo.
#                     R7 mutates refs and only runs on the test repo.
#   FILE_COUNT      - max files sampled per commit (default: 2000)
#   FD_LIMIT        - RLIMIT_NOFILE for git-fs in R5 (default: 128)
#   STORM_P         - parallel readers in R5 (default: 64)
#   MEM_TOL_KB      - allowed RSS growth in R6/R7 (default: 4096)
#   MEM_CHECKS      - 0 = record memory but don't assert (sanitizers)
#   SETTLE_TIMEOUT  - seconds to wait for fds to be released (default: 10)
#   SAMPLE_INTERVAL - sampler period in seconds (default: 0.05)
#   SKIP            - scenarios to skip, e.g. "R5 R7"
#   RESULTS         - also write the metrics as key=value to this file

set -e

. "$(dirname "$0")/lib.sh"
use_gitfs "$1"

FILE_COUNT="${FILE_COUNT:-2000}"
FD_LIMIT="${FD_LIMIT:-128}"
STORM_P="${STORM_P:-64}"
MEM_TOL_KB="${MEM_TOL_KB:-4096}"
MEM_CHECKS="${MEM_CHECKS:-1}"
SETTLE_TIMEOUT="${SETTLE_TIMEOUT:-10}"
SAMPLE_INTERVAL="${SAMPLE_INTERVAL:-0.05}"
SKIP="${SKIP:-}"
RESULTS="${RESULTS:-}"

MNT="$TESTDIR/mnt"
METRICS="$TESTDIR/metrics"
OWN_REPO=""
HOLDERS=""
mkdir "$MNT"

test_cleanup() {
	release_holders
	[ -n "$res_sampler_pid" ] && res_sampler_stop
}

# check_mem NAME STATUS DETAIL [CAUSE]: CAUSE makes it an xcheck.
# MEM_CHECKS=0 skips it: sanitizer quarantines make RSS meaningless.
check_mem() {
	if [ "$MEM_CHECKS" = "1" ] && [ -n "$4" ]; then
		xcheck "$@"
	elif [ "$MEM_CHECKS" = "1" ]; then
		check "$@"
	else
		printf "  %-40s  skipped (MEM_CHECKS=0): %s\n" "$1" "$3"
	fi
}

metric() {
	printf '%s=%s\n' "$1" "$2" >> "$METRICS"
}

skipped() {
	case " $SKIP " in
		*" $1 "*) printf "  %-40s  skipped\n" "$1"; return 0 ;;
	esac
	return 1
}

ratio() {
	awk -v a="$1" -v b="$2" 'BEGIN { printf "%.2f", b ? a / b : 0 }'
}

fds_now() {
	res_fds "$GITFS_PID"
}

rss_now() {
	res_get rss "$(res_mem "$GITFS_PID")"
}

# settle MAX [FIELD]
settle() {
	res_settle "$GITFS_PID" "$1" "$SETTLE_TIMEOUT" "$2" || true
}

list_files() {
	git -C "$REPO" ls-tree -r "$1" \
		| awk -F '\t' '$1 ~ /^100/ { print $2 }' \
		| head -n "$FILE_COUNT" \
		| sed "s|^|$MNT/objects/$1/tree/|"
}

read_files() {
	xargs -d '\n' -P "$2" -n 20 cat < "$1" > /dev/null
}

# a holder that fails to open leaves one line in holders.err
hold_files() {
	while IFS= read -r f; do
		(exec sleep 600 < "$f") 2>> "$TESTDIR/holders.err" &
		HOLDERS="$HOLDERS $!"
	done < "$1"
}

wait_memfds_steady() {
	prev=-1
	i=0
	while [ $i -lt 50 ]; do
		cur=$(res_get memfd "$(fds_now)")
		[ "$cur" -eq "$prev" ] && return 0
		prev=$cur
		sleep 0.1
		i=$((i + 1))
	done
}

# wait_refs_visible HEAD_SHA BRANCH
wait_refs_visible() {
	for _ in $(seq 50); do
		[ -d "$MNT/branches/heads/$2" ] &&
			[ "$(tr -d '\n' < "$MNT/HEAD/hash")" = "$1" ] && return 0
		sleep 0.1
	done
	return 1
}

release_holders() {
	[ -z "$HOLDERS" ] && return 0
	# shellcheck disable=SC2086
	kill $HOLDERS 2>/dev/null || true
	for pid in $HOLDERS; do
		wait "$pid" 2>/dev/null || true
	done
	HOLDERS=""
}

setup_repo() {
	REPO="$TESTDIR/repo"
	OWN_REPO=1
	new_repo "$REPO"

	mkdir -p "$REPO/manyfiles" "$REPO/a/b/c/d"
	i=1
	while [ $i -le 500 ]; do
		printf 'content-%03d\n' "$i" > "$REPO/manyfiles/f-$(printf '%03d' $i).txt"
		i=$((i + 1))
	done

	c=1
	while [ $c -le 3 ]; do
		echo "revision $c" >> "$REPO/a/b/c/d/deep.txt"
		dd if=/dev/urandom bs=4096 count=64 of="$REPO/blob-256k.bin" 2>/dev/null
		commit_all "$REPO" "commit $c"
		c=$((c + 1))
	done

	# real repos are packed: exercise the pack path, not loose objects
	git -C "$REPO" repack -adq
}

echo "test_resources:"

if [ -n "$REPO" ]; then
	echo "  repo: $REPO (external)"
else
	setup_repo
	echo "  repo: $REPO"
fi

C0=$(git -C "$REPO" rev-parse HEAD)
C1=$(git -C "$REPO" rev-parse HEAD~1)
C2=$(git -C "$REPO" rev-parse HEAD~2)

FILES="$TESTDIR/files"
list_files "$C0" > "$FILES"
NFILES=$(wc -l < "$FILES")
echo "  files: $NFILES per commit"
metric files "$NFILES"

mount_gitfs "$REPO" "$MNT"
echo "  mount: $MNT (pid $GITFS_PID)"

# warm up, so libfuse's worker threads and libgit2's packs are part of
# the baseline

read_files "$FILES" 32
ls -lR "$MNT/objects/$C0/tree" > /dev/null
settle 0 memfd

BASE_FDS=$(fds_now)
BASE=$(res_leakable "$BASE_FDS")
BASE_MEM=$(res_mem "$GITFS_PID")
BASE_RSS=$(res_get rss "$BASE_MEM")
metric baseline_fds "$(res_get total "$BASE_FDS")"
metric baseline_fuse_fds "$(res_get fuse "$BASE_FDS")"
metric baseline_pack_fds "$(res_get pack "$BASE_FDS")"
metric baseline_threads "$(res_get threads "$BASE_MEM")"
metric baseline_rss_kb "$BASE_RSS"
printf "  %-40s  %s\n" "R1_baseline" "$BASE_FDS"
check "R1_no_memfds_after_warmup" $(( $(res_get memfd "$BASE_FDS") != 0 )) \
	"$BASE_FDS"

if ! skipped R2; then
	read_files "$FILES" 1
	read_files "$FILES" 8
	xargs -d '\n' -n 50 head -q -c 1 < "$FILES" > /dev/null
	i=0
	while [ $i -lt 50 ]; do
		cat "$MNT/objects/$C0/tree/no-such-file-$i" 2>/dev/null || true
		i=$((i + 1))
	done
	ls -lR "$MNT/objects/$C0/tree" > /dev/null
	ls -l "$MNT/HEAD/" > /dev/null

	settle "$BASE"
	now=$(fds_now)
	check "R2_fds_back_to_baseline" $(( $(res_leakable "$now") != BASE )) \
		"leakable fds $(res_leakable "$now"), baseline $BASE: $now"
	check "R2_no_memfds_left" $(( $(res_get memfd "$now") != 0 )) "$now"
fi

if ! skipped R3; then
	K=64
	head -n "$K" "$FILES" > "$TESTDIR/held"
	hold_files "$TESTDIR/held"
	wait_memfds_steady
	now=$(fds_now)
	per_open=$(ratio $(( $(res_leakable "$now") - BASE )) "$K")
	metric held_files "$K"
	metric fds_per_open_file "$per_open"
	metric memfd_bytes_held "$(res_get memfd_bytes "$now")"
	printf "  %-40s  %s fds per open file (%s)\n" "R3_held_open" "$per_open" "$now"

	release_holders
	settle "$BASE"
	now=$(fds_now)
	check "R3_fds_back_after_close" $(( $(res_leakable "$now") != BASE )) \
		"leakable fds $(res_leakable "$now"), baseline $BASE: $now"
fi

# each reader holds at most one file at a time, so more live memfds
# than readers means FUSE_RELEASE lags behind close()

if ! skipped R4; then
	P=32
	for _ in 1 2 3 4; do cat "$FILES"; done > "$TESTDIR/storm"
	res_sampler_start "$GITFS_PID" "$TESTDIR/r4.log" "$SAMPLE_INTERVAL"
	read_files "$TESTDIR/storm" "$P"
	res_sampler_stop
	peak_memfd=$(res_peak "$TESTDIR/r4.log" memfd)
	metric r4_readers "$P"
	metric r4_peak_fds "$(res_peak "$TESTDIR/r4.log" total)"
	metric r4_peak_memfd "$peak_memfd"
	metric r4_peak_memfd_bytes "$(res_peak "$TESTDIR/r4.log" memfd_bytes)"
	metric r4_release_lag "$(ratio "$peak_memfd" "$P")"
	printf "  %-40s  peak %s memfds for %s readers\n" "R4_release_lag" \
		"$peak_memfd" "$P"

	settle "$BASE"
	now=$(fds_now)
	check "R4_fds_back_after_storm" $(( $(res_leakable "$now") != BASE )) \
		"leakable fds $(res_leakable "$now"), baseline $BASE: $now"
fi

# commits no earlier scenario touched, so pass 1 measures the cost of
# caching new files

if ! skipped R6; then
	list_files "$C1" > "$TESTDIR/r6"
	list_files "$C2" >> "$TESTDIR/r6"
	n6=$(wc -l < "$TESTDIR/r6")
	pass=1
	rss_first=""
	rss_second=""
	rss0=$(rss_now)
	while [ $pass -le 5 ]; do
		read_files "$TESTDIR/r6" 8
		ls -lR "$MNT/objects/$C1/tree" "$MNT/objects/$C2/tree" > /dev/null
		settle "$BASE"
		rss=$(rss_now)
		[ $pass -eq 1 ] && rss_first=$rss
		[ $pass -eq 2 ] && rss_second=$rss
		pass=$((pass + 1))
	done
	growth=$((rss - rss_second))
	metric r6_bytes_per_cached_file $(( (rss_first - rss0) * 1024 / n6 ))
	metric r6_rss_growth_kb "$growth"
	check_mem "R6_rss_plateau" $(( growth >= MEM_TOL_KB )) \
		"RSS grew ${growth}kB from pass 2 to 5 (tolerance ${MEM_TOL_KB}kB)"
fi

if [ -z "$OWN_REPO" ]; then
	printf "  %-40s  skipped (external repo)\n" "R7"
elif ! skipped R7; then
	NB=16
	cycle=1
	rss_second=""
	while [ $cycle -le 10 ]; do
		if [ $((cycle % 2)) -eq 1 ]; then tip=$C1; else tip=$C0; fi
		git -C "$REPO" update-ref HEAD "$tip"
		b=1
		while [ $b -le $NB ]; do
			git -C "$REPO" update-ref "refs/heads/churn-$b" "$tip"
			b=$((b + 1))
		done

		wait_refs_visible "$tip" "churn-$NB" || true

		ls -R "$MNT/HEAD/tree" > /dev/null
		b=1
		while [ $b -le $NB ]; do
			ls -R "$MNT/branches/heads/churn-$b/tree" > /dev/null
			git -C "$REPO" update-ref -d "refs/heads/churn-$b"
			b=$((b + 1))
		done

		settle "$BASE"
		rss=$(rss_now)
		[ $cycle -eq 2 ] && rss_second=$rss
		cycle=$((cycle + 1))
	done

	now=$(fds_now)
	check "R7_fds_back_after_churn" $(( $(res_leakable "$now") != BASE )) \
		"leakable fds $(res_leakable "$now"), baseline $BASE: $now"
	growth=$((rss - rss_second))
	metric r7_rss_growth_kb "$growth"
	check_mem "R7_rss_plateau" $(( growth >= MEM_TOL_KB )) \
		"RSS grew ${growth}kB from cycle 2 to 10 (tolerance ${MEM_TOL_KB}kB)" \
		"push_free() drops the subtree of a retired node"
fi

metric final_rss_kb "$(rss_now)"
metric peak_rss_kb "$(res_get hwm "$(res_mem "$GITFS_PID")")"
umount_gitfs "$MNT"

if ! skipped R5; then
	mount_gitfs "$REPO" "$MNT" "$FD_LIMIT"
	read_files "$FILES" 32
	settle 0 memfd
	base5=$(res_leakable "$(fds_now)")

	K=$((FD_LIMIT * 2))
	for _ in 1 2 3 4 5 6 7 8; do cat "$FILES"; done | head -n "$K" > "$TESTDIR/held"
	rm -f "$TESTDIR/holders.err"
	hold_files "$TESTDIR/held"
	wait_memfds_steady
	failed=$(grep -c . "$TESTDIR/holders.err" || true)
	metric r5_fd_limit "$FD_LIMIT"
	metric r5_held_open_failures "$failed"
	xcheck "R5_hold_2x_fd_limit" $(( failed != 0 )) \
		"$failed of $K opens failed: $(sort -u "$TESTDIR/holders.err" | sed -n 's/.*: //p' | head -1)" \
		"git-fs keeps one memfd per open file"
	release_holders
	settle "$base5"

	for _ in 1 2 3 4; do cat "$FILES"; done > "$TESTDIR/storm"
	status=0
	xargs -d '\n' -P "$STORM_P" -n 50 cat < "$TESTDIR/storm" \
		> /dev/null 2> "$TESTDIR/storm.err" || status=$?
	emfile=$(grep -c 'Too many open files' "$TESTDIR/storm.err" || true)
	metric r5_storm_emfile "$emfile"
	check "R5_storm_no_emfile" $(( status != 0 || emfile != 0 )) \
		"$emfile reads failed with EMFILE (xargs status $status)"

	umount_gitfs "$MNT"
fi

echo
echo "metrics:"
awk -F= '{ printf "  %-40s  %s\n", $1, $2 }' "$METRICS"
[ -n "$RESULTS" ] && cp "$METRICS" "$RESULTS"

if grep -q 'git-fs:' "$GITFS_LOG"; then
	echo
	echo "git-fs stderr:"
	sort "$GITFS_LOG" | uniq -c | head -10
fi

summary
