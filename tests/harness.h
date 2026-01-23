#ifndef HARNESS_H
#define HARNESS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run;
static int tests_failed;

#define FAIL(fmt, ...) do { \
	fprintf(stderr, "  FAIL %s:%d: " fmt "\n", \
	        __FILE__, __LINE__, ##__VA_ARGS__); \
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
		FAIL("%s == \"%s\", expected \"%s\"", #a, _a, _b); \
} while (0)

#define TEST(name) static int name(void)

#define RUN_TEST(fn) do { \
	tests_run++; \
	fprintf(stderr, "  %-40s", #fn); \
	if (fn()) { \
		tests_failed++; \
	} else { \
		fprintf(stderr, "  ok\n"); \
	} \
} while (0)

#define TEST_SUMMARY() do { \
	fprintf(stderr, "\n%d/%d tests passed\n", \
	        tests_run - tests_failed, tests_run); \
	return tests_failed ? EXIT_FAILURE : EXIT_SUCCESS; \
} while (0)

#endif
