# server.q — POSIX TCP listener and accepted socket ownership helpers

@module server
@version 1.0

@param PREFIX server

@include <sys/types.h>
@include <sys/socket.h>
@include <netdb.h>
@include <unistd.h>
@include <fcntl.h>
@include <errno.h>
@include <string.h>
@include <stdio.h>

@guard

@raw {
    typedef struct { int fd; } ${PREFIX}_t;
}

@fn init {
    static inline void ${PREFIX}_init(${PREFIX}_t *s) { s->fd=-1; }
}

@fn nonblocking {
    static inline int ${PREFIX}_nonblocking(int fd, int enabled) {
        int flags=fcntl(fd,F_GETFL,0); if (flags<0) return -1;
        return fcntl(fd,F_SETFL,enabled ? flags|O_NONBLOCK : flags&~O_NONBLOCK);
    }
}

@fn cloexec {
    static inline int ${PREFIX}_cloexec(int fd) {
        int flags=fcntl(fd,F_GETFD,0); if (flags<0) return -1;
        return fcntl(fd,F_SETFD,flags|FD_CLOEXEC);
    }
}

@fn listen {
    static inline int ${PREFIX}_listen(${PREFIX}_t *s, const char *host, unsigned port, int backlog) {
        struct addrinfo hints,*list,*a; char service[6]; int rc,fd=-1,saved=EADDRNOTAVAIL;
        if (!s || s->fd>=0 || port>65535 || backlog<=0) { errno=EINVAL; return -1; }
        memset(&hints,0,sizeof hints); hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_STREAM; hints.ai_flags=AI_PASSIVE;
        snprintf(service,sizeof service,"%u",port); rc=getaddrinfo(host,service,&hints,&list);
        if (rc) { if (rc != EAI_SYSTEM) errno=EADDRNOTAVAIL; return -1; }
        for (a=list;a;a=a->ai_next) {
            int yes=1;
            fd=socket(a->ai_family,a->ai_socktype,a->ai_protocol); if (fd<0) { saved=errno; continue; }
            if (!${PREFIX}_cloexec(fd) && !setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes) &&
                !bind(fd,a->ai_addr,a->ai_addrlen) && !listen(fd,backlog)) break;
            saved=errno; close(fd); fd=-1;
        }
        freeaddrinfo(list);
        if (fd<0) { errno=saved; return -1; }
        s->fd=fd; return 0;
    }
}

@fn accept {
    static inline int ${PREFIX}_accept(const ${PREFIX}_t *s) {
        int fd;
        if (!s || s->fd<0) { errno=EINVAL; return -1; }
        do { fd=accept(s->fd,NULL,NULL); } while (fd<0 && errno==EINTR);
        if (fd>=0 && ${PREFIX}_cloexec(fd)) { int saved=errno; close(fd); errno=saved; return -1; }
        return fd;
    }
}

@fn close {
    static inline int ${PREFIX}_close(${PREFIX}_t *s) {
        int fd;
        if (!s || s->fd<0) return 0;
        fd=s->fd; s->fd=-1; return close(fd);
    }
}
