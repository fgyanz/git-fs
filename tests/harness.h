#ifndef HARNESS_H
#define HARNESS_H

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run;
static int tests_failed;
static int tests_xfailed;
static char fail_msg[1024];

/* RUN_TEST and RUN_XFAIL print the reason */
#define FAIL(...) do { \
	int _n = snprintf(fail_msg, sizeof(fail_msg), "%s:%d: ", \
	                  __FILE__, __LINE__); \
	snprintf(fail_msg + _n, sizeof(fail_msg) - _n, __VA_ARGS__); \
	return 1; \
} while (0)

#define ASSERT(expr) do { \
	if (!(expr)) \
		FAIL("assertion failed: %s", #expr); \
} while (0)

#define ASSERT_EQ(a, b) do { \
	long _a = (long)(a), _b = (long)(b); \
	if (_a != _b) \
		FAIL("%s == %ld, expected %s == %ld", #a, _a, #b, _b); \
} while (0)

#define ASSERT_NOT_NULL(p) do { \
	if ((p) == NULL) \
		FAIL("%s is NULL", #p); \
} while (0)

#define ASSERT_NULL(p) do { \
	if ((p) != NULL) \
		FAIL("%s is not NULL", #p); \
} while (0)

#define ASSERT_STR_EQ(a, b) do { \
	const char *_a = (a), *_b = (b); \
	if (strcmp(_a, _b) != 0) \
		FAIL("%s == \"%.200s\", expected \"%.200s\"", #a, _a, _b); \
} while (0)

#define TEST(name) static int name(void)

static int (*test_setup)(void);
static void (*test_teardown)(void);

static int
run_test(int (*fn)(void), const char *name)
{
	int ret;

	if (test_setup && test_setup()) {
		fprintf(stderr, "  %s: setup failed\n", name);
		exit(EXIT_FAILURE);
	}
	ret = fn();
	if (test_teardown)
		test_teardown();
	return ret;
}

#define RUN_TEST(fn) do { \
	tests_run++; \
	if (run_test(fn, #fn)) { \
		tests_failed++; \
		fprintf(stderr, "  FAIL %-34s  %s\n", #fn, fail_msg); \
	} else { \
		fprintf(stderr, "  %-40s  ok\n", #fn); \
	} \
} while (0)

/* known bug: only an unexpected pass fails the suite */
#define RUN_XFAIL(fn, reason) do { \
	tests_run++; \
	if (run_test(fn, #fn)) { \
		tests_xfailed++; \
		fprintf(stderr, "  XFAIL %-33s  %s\n", #fn, fail_msg); \
		fprintf(stderr, "        %-33s  known: %s\n", "", reason); \
	} else { \
		tests_failed++; \
		fprintf(stderr, "  XPASS %-33s  move to RUN_TEST\n", #fn); \
	} \
} while (0)

#define TEST_SUMMARY() do { \
	fprintf(stderr, "\n%d/%d tests passed", \
	        tests_run - tests_failed - tests_xfailed, tests_run); \
	if (tests_xfailed) \
		fprintf(stderr, " (%d expected failures)", tests_xfailed); \
	fputc('\n', stderr); \
	return tests_failed ? EXIT_FAILURE : EXIT_SUCCESS; \
} while (0)

static inline int
count_open_fds(void)
{
	DIR *d;
	struct dirent *e;
	int n = 0;

	d = opendir("/proc/self/fd");
	if (!d)
		return -1;
	while ((e = readdir(d)))
		if (e->d_name[0] != '.')
			n++;
	closedir(d);

	/* don't count opendir's own fd */
	return n - 1;
}

#endif
