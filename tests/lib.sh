# Shared helpers for the git-fs shell tests. Everything a test creates
# goes under TESTDIR, unmounted and removed on exit after the test's own
# test_cleanup, if it defines one.

TMPBASE="${TMPDIR:-/tmp}"
TESTDIR=$(mktemp -d "$TMPBASE/git-fs-test-XXXXXX")
GITFS_LOG="$TESTDIR/git-fs.log"
GITFS_PID=""
PASS=0
FAIL=0
XFAIL=0

cleanup() {
	if command -v test_cleanup > /dev/null; then
		test_cleanup || true
	fi
	awk -v d="$TESTDIR" 'index($2, d) == 1 { print $2 }' /proc/mounts |
	while IFS= read -r m; do
		fusermount3 -u "$m" 2>/dev/null || true
	done
	rm -rf "$TESTDIR"
}
trap cleanup EXIT

# absolute: tests cd into the repo before mounting
use_gitfs() {
	GITFS="${1:-./git-fs}"
	if [ ! -x "$GITFS" ]; then
		echo "error: $GITFS not found or not executable" >&2
		exit 1
	fi
	GITFS=$(readlink -f "$GITFS")
}

pass() {
	PASS=$((PASS + 1))
	printf "  %-40s  ok\n" "$1"
}

fail() {
	FAIL=$((FAIL + 1))
	printf "  FAIL %-34s  %s\n" "$1" "$2"
}

# check NAME STATUS DETAIL
check() {
	if [ "$2" -eq 0 ]; then pass "$1"; else fail "$1" "$3"; fi
}

# xcheck NAME STATUS DETAIL CAUSE: a known bug. Only an unexpected
# pass fails, so the fix also turns it into a check.
xcheck() {
	if [ "$2" -eq 0 ]; then
		FAIL=$((FAIL + 1))
		printf "  XPASS %-33s  turn into a check\n" "$1"
	else
		XFAIL=$((XFAIL + 1))
		printf "  XFAIL %-33s  %s\n" "$1" "$3"
		printf "        %-33s  known: %s\n" "" "$4"
	fi
}

# assert_*: values first, test name last
assert_eq() {
	if [ "$1" = "$2" ]; then
		pass "$3"
	else
		fail "$3" "got '$1', expected '$2'"
	fi
}

assert_ne() {
	if [ "$1" != "$2" ]; then
		pass "$3"
	else
		fail "$3" "still '$1'"
	fi
}

assert_contains() {
	if echo "$1" | grep -q "$2"; then
		pass "$3"
	else
		fail "$3" "'$2' not found in output"
	fi
}

assert_match() {
	if echo "$1" | grep -qE "$2"; then
		pass "$3"
	else
		fail "$3" "pattern '$2' not matched"
	fi
}

assert_no_match() {
	if echo "$1" | grep -qE "$2"; then
		fail "$3" "pattern '$2' matched"
	else
		pass "$3"
	fi
}

assert_fail() {
	if eval "$1" 2>/dev/null; then
		fail "$2" "command should have failed"
	else
		pass "$2"
	fi
}

summary() {
	echo
	printf "%d/%d tests passed" "$PASS" $((PASS + FAIL + XFAIL))
	[ "$XFAIL" -gt 0 ] && printf " (%d expected failures)" "$XFAIL"
	echo
	[ "$FAIL" -eq 0 ]
}

new_repo() {
	git init -q "$1"
	git -C "$1" config user.email "test@test"
	git -C "$1" config user.name "test"
}

commit_all() {
	git -C "$1" add .
	git -C "$1" commit -q -m "$2"
}

wait_mount() {
	for _ in $(seq 50); do
		ls "$1/HEAD" > /dev/null 2>&1 && return 0
		sleep 0.1
	done
	return 1
}

# mount_gitfs REPO MNT [FD_LIMIT]: in the foreground, so GITFS_PID is
# git-fs itself and its stderr lands in GITFS_LOG
mount_gitfs() {
	(
		[ -n "$3" ] && ulimit -n "$3"
		cd "$1" && exec "$GITFS" -f -m "$2"
	) 2>> "$GITFS_LOG" &
	GITFS_PID=$!
	if ! wait_mount "$2"; then
		echo "error: mount of $1 at $2 not ready after 5s" >&2
		cat "$GITFS_LOG" >&2
		exit 1
	fi
}

umount_gitfs() {
	fusermount3 -u "$1" 2>/dev/null || true
	if [ -n "$GITFS_PID" ]; then
		wait "$GITFS_PID" 2>/dev/null || true
	fi
	GITFS_PID=""
}

# memfd pages are never mapped, so memfd_bytes is memory RSS misses
res_fds() {
	_kinds=$(find "/proc/$1/fd" -type l -printf '%l\n' 2>/dev/null | awk '
		{ total++ }
		/^\/memfd:/             { next }
		/^\/dev\/fuse$/         { fuse++; next }
		/^pipe:/                { pipe++; next }
		/\.(pack|idx|rev)$/     { pack++; next }
		                        { other++ }
		END {
			printf "total=%d fuse=%d pipe=%d pack=%d other=%d",
				total, fuse, pipe, pack, other
		}')
	_memfd=$(find "/proc/$1/fd" -lname '/memfd:*' -exec stat -L -c %s {} + \
		2>/dev/null | awk '{ s += $1 } END { printf "memfd=%d memfd_bytes=%d", NR, s }')
	echo "$_kinds $_memfd"
}

res_mem() {
	awk '
		/^VmRSS:/    { rss = $2 }
		/^RssAnon:/  { anon = $2 }
		/^RssFile:/  { file = $2 }
		/^RssShmem:/ { shmem = $2 }
		/^VmHWM:/    { hwm = $2 }
		/^Threads:/  { threads = $2 }
		END {
			printf "rss=%d anon=%d file=%d shmem=%d hwm=%d threads=%d\n",
				rss, anon, file, shmem, hwm, threads
		}' "/proc/$1/status" 2>/dev/null
}

# res_get FIELD LINE
res_get() {
	echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p"
}

# fds that must go away once clients are done: libfuse never shrinks
# its per-thread /dev/fuse clones and libgit2 keeps packs open
res_leakable() {
	echo $(( $(res_get total "$1") - $(res_get fuse "$1") - $(res_get pack "$1") ))
}

# res_settle PID MAX TIMEOUT [FIELD]: FUSE_RELEASE is asynchronous, so
# git-fs drops its side of a file some time after the client's close()
res_settle() {
	_deadline=$(( $(date +%s) + $3 ))
	while :; do
		_fds=$(res_fds "$1")
		if [ "${4:-leakable}" = leakable ]; then
			_n=$(res_leakable "$_fds")
		else
			_n=$(res_get "$4" "$_fds")
		fi
		[ "$_n" -le "$2" ] && return 0
		[ "$(date +%s)" -ge "$_deadline" ] && return 1
		sleep 0.1
	done
}

# res_sampler_start PID LOG [INTERVAL]
res_sampler_start() {
	_res_sampler_stop="$2.stop"
	rm -f "$_res_sampler_stop"
	(
		while [ ! -f "$_res_sampler_stop" ] && [ -d "/proc/$1" ]; do
			echo "t=$(date +%s.%N) $(res_fds "$1") $(res_mem "$1")"
			sleep "${3:-0.05}"
		done
	) >> "$2" &
	res_sampler_pid=$!
}

res_sampler_stop() {
	: > "$_res_sampler_stop"
	wait "$res_sampler_pid" 2>/dev/null
	rm -f "$_res_sampler_stop"
	res_sampler_pid=""
}

# res_peak LOG FIELD
res_peak() {
	tr ' ' '\n' < "$1" | sed -n "s/^$2=//p" | sort -n | tail -1
}

res_first() {
	head -1 "$1" | tr ' ' '\n' | sed -n "s/^$2=//p"
}

res_last() {
	tail -1 "$1" | tr ' ' '\n' | sed -n "s/^$2=//p"
}
