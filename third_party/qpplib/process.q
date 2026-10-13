# process.q — POSIX argv spawning, waiting, and signals

@module process
@version 1.0

@param PREFIX process

@include <spawn.h>
@include <sys/wait.h>
@include <unistd.h>
@include <signal.h>
@include <errno.h>

@guard

@raw {
    typedef struct { pid_t pid; int live; } ${PREFIX}_t;
}

@fn init {
    static inline void ${PREFIX}_init(${PREFIX}_t *p) { p->pid = -1; p->live = 0; }
}

@fn spawn {
    static inline int ${PREFIX}_spawn(${PREFIX}_t *p, const char *file, char *const argv[], char *const envp[]) {
        int rc; pid_t pid;
        if (!p || p->live || !file || !argv || !argv[0] || !envp) { errno = EINVAL; return -1; }
        rc = posix_spawnp(&pid, file, NULL, NULL, argv, envp);
        if (rc) { errno = rc; return -1; }
        p->pid = pid; p->live = 1; return 0;
    }
}

@fn wait {
    static inline int ${PREFIX}_wait(${PREFIX}_t *p, int *status, int nohang) {
        pid_t result; int st;
        if (!p || !p->live) { errno = ECHILD; return -1; }
        do { result = waitpid(p->pid, &st, nohang ? WNOHANG : 0); } while (result < 0 && errno == EINTR);
        if (result <= 0) return result < 0 ? -1 : 0;
        p->live = 0;
        if (status) *status = st;
        return 1;
    }
}

@fn signal {
    static inline int ${PREFIX}_signal(const ${PREFIX}_t *p, int sig) {
        if (!p || !p->live || p->pid <= 0) { errno = EINVAL; return -1; }
        return kill(p->pid, sig);
    }
}

@fn exit_code {
    static inline int ${PREFIX}_exit_code(int status) {
        return status >= 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
}
