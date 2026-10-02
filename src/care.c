#define _POSIX_C_SOURCE 200809L

#include "care.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* Gregorian calendar <-> day number, after Howard Hinnant's algorithms
 * ("chrono-Compatible Low-Level Date Algorithms"), for year >= 0. */
long care_day(long year, long month, long day)
{
    long y = month <= 2 ? year - 1 : year;
    long era = y / 400;
    long yoe = y - era * 400;
    long doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil(long day, long *year, long *month, long *mday)
{
    long z = day + 719468;
    long era = z / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    *mday = doy - (153 * mp + 2) / 5 + 1;
    *month = mp < 10 ? mp + 3 : mp - 9;
    *year = yoe + era * 400 + (*month <= 2);
}

static int is_leap(long year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static long days_in_month(long year, long month)
{
    static const int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return month == 2 && is_leap(year) ? 29 : days[month - 1];
}

/* Reads exactly n digits; -1 if any is not a digit. */
static long digits(const char *s, int n)
{
    long v = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

int care_parse_date(const char *s, long *day)
{
    if (strlen(s) != 10 || s[4] != '-' || s[7] != '-')
        return -1;
    long y = digits(s, 4), m = digits(s + 5, 2), d = digits(s + 8, 2);
    if (y < CARE_MIN_YEAR || y > CARE_MAX_YEAR || m < 1 || m > 12 || d < 1 ||
        d > days_in_month(y, m))
        return -1;
    *day = care_day(y, m, d);
    return 0;
}

void care_format_date(long day, char out[16])
{
    long y, m, d;
    civil(day, &y, &m, &d);
    snprintf(out, 16, "%04ld-%02ld-%02ld", y, m, d);
}

long care_today(void)
{
    time_t now = time(NULL);
    struct tm tm;
    if (localtime_r(&now, &tm) == NULL) {
        /* Cannot happen for the current time; UTC is a sane fallback. */
        perror("care: localtime_r");
        return (long)(now / 86400);
    }
    return care_day(tm.tm_year + 1900L, tm.tm_mon + 1L, tm.tm_mday);
}

int care_month_day_valid(long month, long day)
{
    /* 2000 is a leap year: Feb 29 is allowed. */
    return month >= 1 && month <= 12 && day >= 1 && day <= days_in_month(2000, month);
}

/* 1 if month/day falls inside period p. */
static int in_period(const struct care_period *p, long month, long day)
{
    long md = month * 100 + day;
    long start = p->start_month * 100 + p->start_day;
    long end = p->end_month * 100 + p->end_day;
    if (start <= end)
        return md >= start && md <= end;
    return md >= start || md <= end; /* wraps the year end */
}

int care_periods_overlap(const struct care_period *a, const struct care_period *b)
{
    /* Two arcs on the year's circle overlap iff one starts inside the other. */
    return in_period(b, a->start_month, a->start_day) ||
           in_period(a, b->start_month, b->start_day);
}

/* The interval in force on day: the period containing it, else the default. */
static int interval_on(const struct care_rule *rule, long day)
{
    long y, m, d;
    civil(day, &y, &m, &d);
    for (size_t i = 0; i < rule->nperiods; i++)
        if (in_period(&rule->periods[i], m, d))
            return rule->periods[i].interval;
    return rule->interval;
}

/* The yearly date in a given year; Feb 29 becomes Feb 28 in other years. */
static long yearly_in(const struct care_rule *rule, long year)
{
    long d = rule->yearly_day;
    if (d > days_in_month(year, rule->yearly_month))
        d = days_in_month(year, rule->yearly_month);
    return care_day(year, rule->yearly_month, d);
}

long care_next_due(const struct care_rule *rule, long created, long last)
{
    long base = last == CARE_NEVER ? created : last;

    if (rule->yearly_month != 0) {
        long y, m, d;
        civil(base, &y, &m, &d);
        long due = yearly_in(rule, y);
        if (last == CARE_NEVER ? due < created : due <= last)
            due = yearly_in(rule, y + 1);
        return due;
    }

    long due = base;
    int interval = interval_on(rule, base);
    if (last != CARE_NEVER && interval > 0)
        due = base + interval;
    /* Skip paused days. Four years and a day covers a rule whose only
     * unpaused day is Feb 29. */
    for (int i = 0; i <= 4 * 366 && interval_on(rule, due) == 0; i++)
        due++;
    return interval_on(rule, due) == 0 ? CARE_NEVER : due;
}
