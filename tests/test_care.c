/* Plant care dates: parsing, periods, and when a rule is next due. */

#include "../src/care.h"
#include "test.h"

/* Day number of a date written "YYYY-MM-DD" (must be valid). */
static long D(const char *s)
{
    long day = CARE_NEVER;
    CHECK(care_parse_date(s, &day) == 0);
    return day;
}

/* The due date of a rule as text, "never" for CARE_NEVER. */
static const char *due(const struct care_rule *r, const char *created, const char *last)
{
    static char out[16];
    long d = care_next_due(r, D(created), last != NULL ? D(last) : CARE_NEVER);
    if (d == CARE_NEVER)
        return "never";
    care_format_date(d, out);
    return out;
}

static void test_parse_date(void)
{
    long d;
    char s[16];

    CHECK(care_parse_date("1970-01-01", &d) == 0 && d == 0);
    CHECK(care_parse_date("1970-01-02", &d) == 0 && d == 1);
    CHECK(care_parse_date("2026-10-02", &d) == 0);
    care_format_date(d, s);
    CHECK_STR(s, "2026-10-02");

    /* leap years */
    CHECK(care_parse_date("2024-02-29", &d) == 0);
    CHECK(care_parse_date("2000-02-29", &d) == 0);
    CHECK(care_parse_date("1900-02-29", &d) == -1);
    CHECK(care_parse_date("2026-02-29", &d) == -1);
    CHECK(care_parse_date("2026-04-31", &d) == -1);
    CHECK(care_parse_date("2026-12-31", &d) == 0);

    /* year bounds */
    CHECK(care_parse_date("1900-01-01", &d) == 0);
    care_format_date(d, s);
    CHECK_STR(s, "1900-01-01");
    CHECK(care_parse_date("1899-12-31", &d) == -1);
    CHECK(care_parse_date("9999-12-31", &d) == 0);
    care_format_date(d, s);
    CHECK_STR(s, "9999-12-31");

    /* shape */
    CHECK(care_parse_date("", &d) == -1);
    CHECK(care_parse_date("2026-1-02", &d) == -1);
    CHECK(care_parse_date("2026-10-2", &d) == -1);
    CHECK(care_parse_date("2026-10-021", &d) == -1);
    CHECK(care_parse_date("2026/10/02", &d) == -1);
    CHECK(care_parse_date("2026-00-10", &d) == -1);
    CHECK(care_parse_date("2026-13-10", &d) == -1);
    CHECK(care_parse_date("2026-10-00", &d) == -1);
    CHECK(care_parse_date("+026-10-02", &d) == -1);
    CHECK(care_parse_date("2026-1a-02", &d) == -1);
    CHECK(care_parse_date(" 2026-10-0", &d) == -1);

    /* every day of a leap and a common year round-trips */
    for (long day = D("2023-01-01"); day <= D("2024-12-31"); day++) {
        long back;
        care_format_date(day, s);
        CHECK(care_parse_date(s, &back) == 0 && back == day);
    }
}

static void test_month_day(void)
{
    CHECK(care_month_day_valid(1, 1));
    CHECK(care_month_day_valid(2, 29));
    CHECK(care_month_day_valid(12, 31));
    CHECK(!care_month_day_valid(2, 30));
    CHECK(!care_month_day_valid(4, 31));
    CHECK(!care_month_day_valid(0, 1));
    CHECK(!care_month_day_valid(13, 1));
    CHECK(!care_month_day_valid(1, 0));
    CHECK(!care_month_day_valid(1, 32));
}

static void test_overlap(void)
{
    struct care_period winter = { 11, 1, 2, 28, 0 };  /* wraps the year end */
    struct care_period spring = { 3, 1, 5, 31, 7 };
    struct care_period march1 = { 3, 1, 3, 1, 7 };    /* a single day */
    struct care_period feb = { 2, 1, 2, 10, 7 };
    struct care_period dec = { 12, 1, 12, 31, 7 };
    struct care_period feb28 = { 2, 28, 3, 1, 7 };

    CHECK(!care_periods_overlap(&winter, &spring));
    CHECK(!care_periods_overlap(&spring, &winter));
    CHECK(care_periods_overlap(&winter, &feb));
    CHECK(care_periods_overlap(&feb, &winter));
    CHECK(care_periods_overlap(&winter, &dec));
    CHECK(care_periods_overlap(&dec, &winter));
    CHECK(care_periods_overlap(&spring, &march1));
    CHECK(care_periods_overlap(&march1, &march1));
    CHECK(care_periods_overlap(&winter, &feb28)); /* share Feb 28 */
    CHECK(care_periods_overlap(&feb28, &spring)); /* share Mar 1 */
    CHECK(!care_periods_overlap(&feb, &dec));
}

static void test_interval(void)
{
    struct care_rule every4 = { .interval = 4 };

    /* never logged: due the day the rule was created */
    CHECK_STR(due(&every4, "2026-10-02", NULL), "2026-10-02");
    /* logged: last + interval, across month and year ends */
    CHECK_STR(due(&every4, "2026-01-01", "2026-10-02"), "2026-10-06");
    CHECK_STR(due(&every4, "2026-01-01", "2026-12-30"), "2027-01-03");
    CHECK_STR(due(&every4, "2024-01-01", "2024-02-27"), "2024-03-02");

    /* a log from before the rule was made still counts */
    CHECK_STR(due(&every4, "2026-10-02", "2026-09-01"), "2026-09-05");

    struct care_rule max = { .interval = CARE_MAX_INTERVAL };
    CHECK_STR(due(&max, "2026-01-01", "2026-01-01"), "2035-12-30");
}

static void test_periods(void)
{
    /* water every 4 days; every 10 Nov 1 - Feb 28; paused Jul 1 - Jul 31 */
    struct care_period p[] = {
        { 11, 1, 2, 28, 10 },
        { 7, 1, 7, 31, 0 },
    };
    struct care_rule r = { .interval = 4, .periods = p, .nperiods = 2 };

    CHECK_STR(due(&r, "2026-01-01", "2026-10-02"), "2026-10-06");
    /* the interval in force on the day it was last done */
    CHECK_STR(due(&r, "2026-01-01", "2026-10-31"), "2026-11-04");
    CHECK_STR(due(&r, "2026-01-01", "2026-11-01"), "2026-11-11");
    CHECK_STR(due(&r, "2026-01-01", "2026-12-28"), "2027-01-07");
    CHECK_STR(due(&r, "2026-01-01", "2027-02-28"), "2027-03-10");
    /* Feb 29 is outside "Nov 1 - Feb 28": the default applies */
    CHECK_STR(due(&r, "2024-01-01", "2024-02-29"), "2024-03-04");

    /* due inside the pause: moves to the first day after it */
    CHECK_STR(due(&r, "2026-01-01", "2026-06-29"), "2026-08-01");
    CHECK_STR(due(&r, "2026-01-01", "2026-06-27"), "2026-08-01"); /* Jul 1 is paused */
    CHECK_STR(due(&r, "2026-01-01", "2026-06-26"), "2026-06-30");
    /* done during the pause (e.g. once in July): due when it ends */
    CHECK_STR(due(&r, "2026-01-01", "2026-07-15"), "2026-08-01");
    /* created during the pause, never done */
    CHECK_STR(due(&r, "2026-07-10", NULL), "2026-08-01");
    CHECK_STR(due(&r, "2026-06-30", NULL), "2026-06-30");
}

static void test_paused_default(void)
{
    /* fertilise only Apr 1 - Sep 30, every 14 days; paused otherwise */
    struct care_period p[] = { { 4, 1, 9, 30, 14 } };
    struct care_rule r = { .interval = 0, .periods = p, .nperiods = 1 };

    CHECK_STR(due(&r, "2026-01-10", NULL), "2026-04-01");
    CHECK_STR(due(&r, "2026-01-10", "2026-04-01"), "2026-04-15");
    CHECK_STR(due(&r, "2026-01-10", "2026-09-20"), "2027-04-01");
    CHECK_STR(due(&r, "2026-01-10", "2026-09-30"), "2027-04-01");

    /* paused every day of the year */
    struct care_period all[] = { { 1, 1, 12, 31, 0 } };
    struct care_rule never = { .interval = 5, .periods = all, .nperiods = 1 };
    CHECK_STR(due(&never, "2026-01-10", NULL), "never");
    CHECK_STR(due(&never, "2026-01-10", "2026-03-03"), "never");

    /* only Feb 29 is not paused: found up to four years ahead */
    struct care_period most[] = { { 3, 1, 2, 28, 0 } };
    struct care_rule leap = { .interval = 0, .periods = most, .nperiods = 1 };
    struct care_period feb29[] = { { 3, 1, 2, 28, 0 }, { 2, 29, 2, 29, 1 } };
    struct care_rule leap2 = { .interval = 0, .periods = feb29, .nperiods = 2 };
    CHECK_STR(due(&leap, "2025-03-01", NULL), "never"); /* default paused too */
    CHECK_STR(due(&leap2, "2025-03-01", NULL), "2028-02-29");
}

static void test_yearly(void)
{
    struct care_rule repot = { .yearly_month = 3, .yearly_day = 1 };

    /* never done: on or after the day the rule was created */
    CHECK_STR(due(&repot, "2026-01-15", NULL), "2026-03-01");
    CHECK_STR(due(&repot, "2026-03-01", NULL), "2026-03-01");
    CHECK_STR(due(&repot, "2026-03-02", NULL), "2027-03-01");
    /* done: strictly after the last time */
    CHECK_STR(due(&repot, "2020-01-01", "2026-03-01"), "2027-03-01");
    CHECK_STR(due(&repot, "2020-01-01", "2026-02-20"), "2026-03-01");
    CHECK_STR(due(&repot, "2020-01-01", "2026-03-20"), "2027-03-01");
    CHECK_STR(due(&repot, "2020-01-01", "2026-12-31"), "2027-03-01");

    /* Feb 29 falls on Feb 28 in common years */
    struct care_rule leap = { .yearly_month = 2, .yearly_day = 29 };
    CHECK_STR(due(&leap, "2025-01-01", NULL), "2025-02-28");
    CHECK_STR(due(&leap, "2020-01-01", "2025-02-28"), "2026-02-28");
    CHECK_STR(due(&leap, "2020-01-01", "2027-03-01"), "2028-02-29");

    struct care_rule nye = { .yearly_month = 12, .yearly_day = 31 };
    CHECK_STR(due(&nye, "2020-01-01", "2026-12-31"), "2027-12-31");
}

int main(void)
{
    test_parse_date();
    test_month_day();
    test_overlap();
    test_interval();
    test_periods();
    test_paused_default();
    test_yearly();
    TEST_DONE();
}
