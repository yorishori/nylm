#ifndef ACTION_H
#define ACTION_H

/*
 * Root actions: the scripts in /usr/local/lib/nylm/actions/ (from
 * deploy/actions/), run through `sudo -n`, never through a shell. name must
 * be a string constant in the code, never request text.
 */

/* Runs the action without arguments and waits for it (at most 30 s).
 * 0 if it exited with 0, else -1 (logged). */
int action_run(const char *name);

#endif
