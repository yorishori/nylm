/* Running commands: output, its limit, exit status, timeout; action arguments. */

#include "../src/action.h"
#include "../src/arena.h"
#include "test.h"

static void test_output(void)
{
    char *out = NULL;
    size_t len = 0;
    int status = 1;
    char printf_[] = "/usr/bin/printf", fmt[] = "a\\nb";
    char *const say[] = { printf_, fmt, NULL };
    CHECK(command_output(say, 5, 16, &out, &len, &status) == 0);
    CHECK_STR(out, "a\nb");
    CHECK(len == 3);
    CHECK(status == 0);

    /* Exactly max bytes is fine, one more is a failure. */
    CHECK(command_output(say, 5, 3, &out, &len, NULL) == 0);
    CHECK(len == 3);
    out = NULL;
    CHECK(command_output(say, 5, 2, &out, &len, NULL) == -1);
    CHECK(out == NULL);

    /* No output at all; output not kept. */
    char true_[] = "/usr/bin/true";
    char *const nothing[] = { true_, NULL };
    CHECK(command_output(nothing, 5, 16, &out, &len, NULL) == 0);
    CHECK_STR(out, "");
    CHECK(len == 0);
    CHECK(command_output(nothing, 5, 0, NULL, NULL, NULL) == 0);

    /* Large output, read in many pieces. */
    char head[] = "/usr/bin/head", c[] = "-c", n[] = "200000", zero[] = "/dev/zero";
    char *const big[] = { head, c, n, zero, NULL };
    CHECK(command_output(big, 5, 200000, &out, &len, NULL) == 0);
    CHECK(len == 200000);
}

static void test_failing(void)
{
    int status = 0;
    char sh[] = "/usr/bin/sh", dash_c[] = "-c", exit2[] = "exit 2";
    char *const two[] = { sh, dash_c, exit2, NULL };
    CHECK(command_output(two, 5, 0, NULL, NULL, &status) == -1);
    CHECK(status == 2);

    char missing[] = "/nonexistent/nylm-test";
    char *const nope[] = { missing, NULL };
    CHECK(command_output(nope, 5, 0, NULL, NULL, &status) == -1);
    CHECK(status == 127);

    /* Too slow: stopped at the timeout, with or without output kept. */
    char sleep_[] = "/usr/bin/sleep", ten[] = "10";
    char *const slow[] = { sleep_, ten, NULL };
    char *out = NULL;
    CHECK(command_output(slow, 1, 16, &out, NULL, &status) == -1);
    CHECK(status == -1);
    CHECK(out == NULL);
    CHECK(command_output(slow, 1, 0, NULL, NULL, &status) == -1);

    /* stdin is /dev/null: a command reading it ends at once. */
    char cat[] = "/usr/bin/cat";
    char *const reads[] = { cat, NULL };
    CHECK(command_output(reads, 2, 16, &out, NULL, NULL) == 0);
    CHECK_STR(out, "");
}

static void test_args(void)
{
    CHECK(action_arg_valid("davis"));
    CHECK(action_arg_valid("wg-quick@wg0.service"));
    CHECK(action_arg_valid("a_b.c:d-1"));
    CHECK(!action_arg_valid(NULL));
    CHECK(!action_arg_valid(""));
    CHECK(!action_arg_valid("-n"));
    CHECK(!action_arg_valid("a b"));
    CHECK(!action_arg_valid("a/b"));
    CHECK(!action_arg_valid("a;b"));
    CHECK(!action_arg_valid("a\nb"));
    CHECK(!action_arg_valid("$(x)"));
    char s[130];
    memset(s, 'a', sizeof s);
    s[128] = '\0';
    CHECK(action_arg_valid(s)); /* 128: the most */
    s[128] = 'a';
    s[129] = '\0';
    CHECK(!action_arg_valid(s));

    /* An invalid argument never reaches sudo. */
    CHECK(action_run("nylm-test", "-x") == -1);
}

int main(void)
{
    if (arena_init(1024 * 1024) != 0)
        return 1;
    test_output();
    test_failing();
    test_args();
    TEST_DONE();
}
