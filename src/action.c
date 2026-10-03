#define _POSIX_C_SOURCE 200809L

#include "action.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ACTIONS_DIR     "/usr/local/lib/nylm/actions/"
#define ACTION_TIMEOUT  30 /* seconds */

int action_run(const char *name)
{
    char path[256];
    int n = snprintf(path, sizeof path, ACTIONS_DIR "%s", name);
    if (n < 0 || (size_t)n >= sizeof path) {
        fprintf(stderr, "action %s: name too long\n", name);
        return -1;
    }
    char sudo[] = "sudo", nonint[] = "-n", env_path[] = "PATH=/usr/bin";
    char *const argv[] = { sudo, nonint, path, NULL };
    char *const envp[] = { env_path, NULL };

    /* Anything buffered would otherwise be written twice. */
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "action %s: fork: %s\n", name, strerror(errno));
        return -1;
    }
    if (pid == 0) {
        signal(SIGPIPE, SIG_DFL); /* the server ignores it; the action must not */
        execve("/usr/bin/sudo", argv, envp);
        _exit(127);
    }

    const struct timespec tick = { 0, 100 * 1000 * 1000 };
    for (int i = 0; i < ACTION_TIMEOUT * 10; i++) {
        int status;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
                return 0;
            fprintf(stderr, "action %s failed (%s %d)\n", name,
                    WIFEXITED(status) ? "exit status" : "signal",
                    WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
            return -1;
        }
        if (r < 0 && errno != EINTR) {
            fprintf(stderr, "action %s: waitpid: %s\n", name, strerror(errno));
            return -1;
        }
        nanosleep(&tick, NULL);
    }
    fprintf(stderr, "action %s: no result after %d s, killed\n", name, ACTION_TIMEOUT);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
}
