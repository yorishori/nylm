#ifndef TEST_H
#define TEST_H

/* Minimal test helpers: CHECK records failures, TEST_DONE reports them. */

#include <stdio.h>
#include <string.h>

static int test_failures;
static int test_checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        test_checks++;                                                       \
        if (!(cond)) {                                                       \
            test_failures++;                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
        }                                                                    \
    } while (0)

#define CHECK_STR(a, b) CHECK((a) != NULL && strcmp((a), (b)) == 0)

#define TEST_DONE()                                                          \
    do {                                                                     \
        printf("%-24s %d checks, %d failed\n", __FILE__, test_checks,       \
               test_failures);                                               \
        return test_failures == 0 ? 0 : 1;                                   \
    } while (0)

#endif
