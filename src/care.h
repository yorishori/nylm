#ifndef CARE_H
#define CARE_H

#include <stddef.h>

/*
 * Plant care scheduling: calendar dates and when a care rule is next due.
 * Pure functions, no database. Dates are day numbers (days since 1970-01-01)
 * and are written "YYYY-MM-DD".
 */

#define CARE_MIN_YEAR      1900
#define CARE_MAX_YEAR      9999
#define CARE_MAX_INTERVAL  3650 /* days */
#define CARE_MAX_PERIODS   12   /* seasonal periods per rule */
#define CARE_NEVER         (-1000000L) /* "no due date": paused all year */

/*
 * Colours for plants and care types: pastels that stay clear of the two
 * urgency colours (late, today) the frontend uses for due dates.
 */
#define CARE_COLOR_NAMES "butter, lime, mint, teal, sky, periwinkle, lavender, orchid"

/* 1 if name is one of CARE_COLOR_NAMES. */
int care_color_valid(const char *name);

/* Parses a valid calendar date "YYYY-MM-DD" (CARE_MIN_YEAR..CARE_MAX_YEAR). 0 or -1. */
int care_parse_date(const char *s, long *day);

/* Writes day as "YYYY-MM-DD" (out must hold 16 bytes). */
void care_format_date(long day, char out[16]);

/* Today in the server's local time zone. */
long care_today(void);

/* 1 if month/day can exist in some year (Feb 29 allowed). */
int care_month_day_valid(long month, long day);

/* Day number of year-month-day (no validation, year >= 0). */
long care_day(long year, long month, long day);

/*
 * A yearly window, start and end inclusive. It wraps the year end when start
 * is after end (e.g. Nov 1 - Feb 28). interval 0 means paused.
 */
struct care_period {
    int start_month, start_day, end_month, end_day;
    int interval;
};

/* 1 if the two periods share at least one month/day. */
int care_periods_overlap(const struct care_period *a, const struct care_period *b);

/*
 * A care rule: either every `interval` days (0 = paused, except inside
 * periods that set an interval), or once a year on yearly_month/yearly_day.
 */
struct care_rule {
    int interval;
    int yearly_month, yearly_day; /* 0 when not yearly */
    const struct care_period *periods;
    size_t nperiods;
};

/*
 * Next due day of a rule created on `created`, last done on `last`
 * (CARE_NEVER if never). Never logged: due on the creation day. Yearly: the
 * date on or after creation, or strictly after the last time (Feb 29 is
 * Feb 28 in other years). Interval: last + the interval in force on `last`.
 * Nothing comes due while paused: it moves to the first unpaused day.
 * Returns CARE_NEVER if the rule is paused all year.
 */
long care_next_due(const struct care_rule *rule, long created, long last);

#endif
