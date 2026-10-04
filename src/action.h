#ifndef ACTION_H
#define ACTION_H

#include <stddef.h>

/*
 * Root actions: the scripts in /usr/local/lib/nylm/actions/ (from
 * deploy/actions/), run through `sudo -n`, never through a shell. name must
 * be a string constant in the code, never request text. arg is NULL or one
 * argument the caller checked against the same rule as the script; it must
 * also pass action_arg_valid().
 */

#define ACTION_TIMEOUT 30 /* seconds */

/* Runs the action and waits for it (at most ACTION_TIMEOUT).
 * 0 if it exited with 0, else -1 (logged). */
int action_run(const char *name, const char *arg);

/*
 * Like action_run, and its standard output (at most max bytes) goes to
 * *out, NUL-terminated, in the request arena, its length to *len. More
 * output than max is a failure.
 */
int action_output(const char *name, const char *arg, size_t max, char **out, size_t *len);

/*
 * Runs a command that needs no root (argv[0] an absolute path, never a
 * shell) with PATH=/usr/bin and stdin from /dev/null, for at most timeout
 * seconds; its output as for action_output (out NULL: not kept). 0 if it
 * exited with 0, else -1 (logged). *status gets the exit status (-1 if it
 * did not exit) when status is not NULL, so a caller can accept others.
 */
int command_output(char *const argv[], int timeout, size_t max, char **out, size_t *len,
                   int *status);

/* 1 if s may be an action's argument: 1..128 bytes of A-Z a-z 0-9 _ . @ : -,
 * not starting with '-'. Each action checks a stricter rule of its own. */
int action_arg_valid(const char *s);

#endif
