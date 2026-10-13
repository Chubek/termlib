# evloop.q — single-threaded poll reactor with callback-safe registration changes

@module evloop
@version 1.0

@param PREFIX evloop
@param CAP 64

@require {$CAP > 0} "CAP must be positive"

@include <poll.h>
@include <stdint.h>
@include <string.h>
@include <errno.h>

@guard

@raw {
    typedef void (*${PREFIX}_callback)(int fd, short events, void *ctx);
    typedef struct { int fd; short events; ${PREFIX}_callback fn; void *ctx; uint64_t generation; } ${PREFIX}_slot;
    typedef struct { ${PREFIX}_slot slots[${CAP}]; uint64_t serial; int stopped, dispatching; } ${PREFIX}_t;
}

@fn init {
    static inline void ${PREFIX}_init(${PREFIX}_t *loop) {
        size_t i; memset(loop,0,sizeof *loop);
        for (i=0;i<${CAP};++i) loop->slots[i].fd=-1;
    }
}

@fn remove {
    static inline int ${PREFIX}_remove(${PREFIX}_t *loop, int fd) {
        size_t i;
        for (i=0;i<${CAP};++i) if (loop->slots[i].fd == fd && fd >= 0) { loop->slots[i].fd=-1; return 1; }
        return 0;
    }
}

@fn add {
    static inline int ${PREFIX}_add(${PREFIX}_t *loop, int fd, short events, ${PREFIX}_callback fn, void *ctx) {
        size_t i,slot=${CAP};
        if (fd < 0 || !fn || loop->serial == UINT64_MAX) return 0;
        for (i=0;i<${CAP};++i) {
            if (loop->slots[i].fd == fd) { slot=i; break; }
            if (loop->slots[i].fd < 0 && slot == ${CAP}) slot=i;
        }
        if (slot == ${CAP}) return 0;
        loop->slots[slot].fd=fd; loop->slots[slot].events=events;
        loop->slots[slot].fn=fn; loop->slots[slot].ctx=ctx;
        loop->slots[slot].generation=++loop->serial; return 1;
    }
}

@fn stop {
    static inline void ${PREFIX}_stop(${PREFIX}_t *loop) { loop->stopped=1; }
}

@fn once {
    static inline int ${PREFIX}_once(${PREFIX}_t *loop, int timeout_ms) {
        struct pollfd fds[${CAP}]; uint64_t generations[${CAP}]; size_t indices[${CAP}];
        size_t i,n=0; int rc,called=0;
        if (loop->dispatching) { errno=EBUSY; return -1; }
        for (i=0;i<${CAP};++i) if (loop->slots[i].fd >= 0) {
            fds[n].fd=loop->slots[i].fd; fds[n].events=loop->slots[i].events; fds[n].revents=0;
            generations[n]=loop->slots[i].generation; indices[n++]=i;
        }
        /* EINTR is returned to the caller so finite timeouts are not restarted. */
        rc=poll(fds,(nfds_t)n,timeout_ms); if (rc <= 0) return rc;
        loop->dispatching=1;
        for (i=0;i<n && !loop->stopped;++i) {
            ${PREFIX}_slot *s=&loop->slots[indices[i]];
            if (fds[i].revents && s->fd == fds[i].fd && s->generation == generations[i]) {
                s->fn(s->fd,fds[i].revents,s->ctx); ++called;
            }
        }
        loop->dispatching=0; return called;
    }
}

@fn run {
    static inline int ${PREFIX}_run(${PREFIX}_t *loop, int timeout_ms) {
        if (loop->dispatching) { errno=EBUSY; return -1; }
        loop->stopped=0;
        while (!loop->stopped) if (${PREFIX}_once(loop,timeout_ms)<0 && errno != EINTR) return -1;
        return 0;
    }
}
