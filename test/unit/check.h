#ifndef CHECK_H
#define CHECK_H
#include <stdio.h>
#include <string.h>

static int check_failures = 0;
static int check_count = 0;

#define CHECK(cond) do { \
    check_count++; \
    if (!(cond)) { \
        check_failures++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_EQ_INT(a, b) do { \
    long long check_a_ = (long long)(a), check_b_ = (long long)(b); \
    check_count++; \
    if (check_a_ != check_b_) { \
        check_failures++; \
        fprintf(stderr, "FAIL %s:%d: %s == %s (%lld != %lld)\n", \
                __FILE__, __LINE__, #a, #b, check_a_, check_b_); \
    } \
} while (0)

#define CHECK_EQ_STR(a, b) do { \
    const char *check_a_ = (a), *check_b_ = (b); \
    check_count++; \
    if (strcmp(check_a_, check_b_) != 0) { \
        check_failures++; \
        fprintf(stderr, "FAIL %s:%d: %s == %s (\"%s\" != \"%s\")\n", \
                __FILE__, __LINE__, #a, #b, check_a_, check_b_); \
    } \
} while (0)

#define CHECK_REPORT(name) do { \
    fprintf(stderr, "%s: %d checks, %d failures\n", name, check_count, check_failures); \
    return check_failures == 0 ? 0 : 1; \
} while (0)

#endif
