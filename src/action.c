#define _POSIX_C_SOURCE 200809L

#include "action.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "arena.h"

#define ACTIONS_DIR  "/usr/local/lib/nylm/actions/"
#define ARG_MAX_LEN  128
#define TERM_GRACE_MS 2000 /* after SIGTERM, before SIGKILL */

int action_arg_valid(const char *s)
{
    if (s == NULL || s[0] == '-')
        return 0;
    size_t n = 0;
    for (; s[n] != '\0'; n++) {
        char c = s[n];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == '_' || c == '.' || c == '@' || c == ':' || c == '-';
        if (!ok || n >= ARG_MAX_LEN)
            return 0;
    }
    return n > 0;
}

static long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void sleep_ms(long ms)
{
    const struct timespec t = { ms / 1000, (ms % 1000) * 1000 * 1000 };
    nanosleep(&t, NULL);
}

/* Waits for pid until deadline (ms). 1 and *st when it ended, 0 if not. */
static int wait_until(pid_t pid, long long deadline, int *st)
{
    for (;;) {
        pid_t r = waitpid(pid, st, WNOHANG);
        if (r == pid)
            return 1;
        if (r < 0 && errno != EINTR)
            return 0;
        if (now_ms() >= deadline)
            return 0;
        sleep_ms(10);
    }
}

/* Stops pid: SIGTERM (sudo passes it on to the action), then SIGKILL. */
static void stop_child(pid_t pid)
{
    int st;
    kill(pid, SIGTERM);
    if (!wait_until(pid, now_ms() + TERM_GRACE_MS, &st)) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }
}

/* Reads the child's output from fd into buf (max bytes) until EOF or the
 * deadline. NULL when done, else why it failed. */
static const char *read_output(int fd, char *buf, size_t max, size_t *used, long long deadline)
{
    for (;;) {
        long long left = deadline - now_ms();
        if (left <= 0)
            return "no result in time, stopped";
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int r = poll(&p, 1, left > 1000 ? 1000 : (int)left);
        if (r < 0 && errno != EINTR)
            return "poll failed";
        if (r <= 0)
            continue;
        /* One byte more than max: to see that there is too much. */
        ssize_t n = read(fd, buf + *used, max + 1 - *used);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return "read failed";
        }
        if (n == 0)
            return NULL;
        *used += (size_t)n;
        if (*used > max)
            return "too much output, stopped";
    }
}

/* command_output, with what to call it in the log. */
static int run(const char *what, char *const argv[], int timeout, size_t max, char **out,
               size_t *len, int *status)
{
    char env_path[] = "PATH=/usr/bin";
    char *const envp[] = { env_path, NULL };
    int fds[2] = { -1, -1 };
    char *buf = NULL;
    size_t used = 0;

    if (status != NULL)
        *status = -1;
    if (out != NULL) {
        buf = arena_alloc(max + 1);
        if (buf == NULL) {
            fprintf(stderr, "%s: out of memory\n", what);
            return -1;
        }
        if (pipe(fds) != 0) {
            fprintf(stderr, "%s: pipe: %s\n", what, strerror(errno));
            return -1;
        }
    }
    int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (devnull < 0) {
        fprintf(stderr, "%s: /dev/null: %s\n", what, strerror(errno));
        if (out != NULL) {
            close(fds[0]);
            close(fds[1]);
        }
        return -1;
    }

    /* Anything buffered would otherwise be written twice. */
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGPIPE, SIG_DFL); /* the server ignores it; the command must not */
        if (dup2(devnull, STDIN_FILENO) < 0 ||
            (out != NULL && dup2(fds[1], STDOUT_FILENO) < 0))
            _exit(127);
        if (out != NULL) {
            close(fds[0]);
            close(fds[1]);
        }
        execve(argv[0], argv, envp);
        _exit(127);
    }
    int fork_errno = errno;
    close(devnull);
    if (out != NULL)
        close(fds[1]);
    if (pid < 0) {
        fprintf(stderr, "%s: fork: %s\n", what, strerror(fork_errno));
        if (out != NULL)
            close(fds[0]);
        return -1;
    }

    long long deadline = now_ms() + (long long)timeout * 1000;
    const char *why = NULL;
    if (out != NULL) {
        why = read_output(fds[0], buf, max, &used, deadline);
        close(fds[0]);
    }
    int st;
    if (why == NULL && !wait_until(pid, deadline, &st))
        why = "no result in time, stopped";
    if (why != NULL) {
        stop_child(pid);
        fprintf(stderr, "%s: %s (after at most %d s)\n", what, why, timeout);
        return -1;
    }
    if (!WIFEXITED(st)) {
        fprintf(stderr, "%s failed (signal %d)\n", what, WTERMSIG(st));
        return -1;
    }
    if (status != NULL)
        *status = WEXITSTATUS(st);
    if (WEXITSTATUS(st) != 0) {
        fprintf(stderr, "%s failed (exit status %d)\n", what, WEXITSTATUS(st));
        return -1;
    }
    if (out != NULL) {
        buf[used] = '\0';
        *out = buf;
        if (len != NULL)
            *len = used;
    }
    return 0;
}

int command_output(char *const argv[], int timeout, size_t max, char **out, size_t *len,
                   int *status)
{
    return run(argv[0], argv, timeout, max, out, len, status);
}

int action_output(const char *name, const char *arg, size_t max, char **out, size_t *len)
{
    char path[256];
    int n = snprintf(path, sizeof path, ACTIONS_DIR "%s", name);
    if (n < 0 || (size_t)n >= sizeof path) {
        fprintf(stderr, "action %s: name too long\n", name);
        return -1;
    }
    if (arg != NULL && !action_arg_valid(arg)) {
        fprintf(stderr, "action %s: invalid argument\n", name);
        return -1;
    }
    char *arg_copy = arg != NULL ? arena_strndup(arg, strlen(arg)) : NULL;
    if (arg != NULL && arg_copy == NULL) {
        fprintf(stderr, "action %s: out of memory\n", name);
        return -1;
    }
    char what[160];
    snprintf(what, sizeof what, "action %s", name);
    char sudo[] = "/usr/bin/sudo", nonint[] = "-n";
    char *const argv[] = { sudo, nonint, path, arg_copy, NULL };
    return run(what, argv, ACTION_TIMEOUT, max, out, len, NULL);
}

int action_run(const char *name, const char *arg)
{
    return action_output(name, arg, 0, NULL, NULL);
}
