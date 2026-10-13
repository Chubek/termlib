# daemon.q — POSIX detach and advisory pidfile locking

@module daemon
@version 1.0

@param PREFIX daemon

@include <unistd.h>
@include <fcntl.h>
@include <sys/stat.h>
@include <sys/types.h>
@include <stdio.h>
@include <errno.h>

@guard

@raw {
    /* Call before creating threads. Returns 1 in original parent (caller exits),
     * 0 in daemon, -1 on failure. The intermediate child exits internally.
     * Parent return is not a readiness notification. Inherited fds remain open. */
}

@fn detach {
    static inline int ${PREFIX}_detach(int change_dir, int redirect_stdio) {
        pid_t pid=fork(); int fd;
        if (pid<0) return -1;
        if (pid>0) return 1;
        if (setsid()<0) return -1;
        pid=fork(); if (pid<0) return -1;
        if (pid>0) _exit(0);
        if (change_dir && chdir("/")) return -1;
        if (!redirect_stdio) return 0;
        fd=open("/dev/null",O_RDWR); if (fd<0) return -1;
        if (dup2(fd,0)<0 || dup2(fd,1)<0 || dup2(fd,2)<0) {
            int saved=errno; if (fd>2) close(fd); errno=saved; return -1;
        }
        if (fd>2) close(fd);
        return 0;
    }
}

@fn pidfile_lock {
    static inline int ${PREFIX}_pidfile_lock(const char *path) {
        int fd, saved, n; struct flock lock; char buf[64]; size_t done=0;
        fd=open(path,O_RDWR|O_CREAT|O_CLOEXEC,0600); if (fd<0) return -1;
        lock.l_type=F_WRLCK; lock.l_whence=SEEK_SET; lock.l_start=0; lock.l_len=0; lock.l_pid=0;
        if (fcntl(fd,F_SETLK,&lock)<0 || ftruncate(fd,0)<0) { saved=errno; close(fd); errno=saved; return -1; }
        n=snprintf(buf,sizeof buf,"%ld\\n",(long)getpid());
        if (n<0 || (size_t)n>=sizeof buf) { close(fd); errno=EIO; return -1; }
        while (done<(size_t)n) {
            ssize_t written=write(fd,buf+done,(size_t)n-done);
            if (written<0 && errno==EINTR) continue;
            if (written<=0) { saved=written<0 ? errno : EIO; close(fd); errno=saved; return -1; }
            done+=(size_t)written;
        }
        return fd;
    }
}

@fn pidfile_unlock {
    static inline int ${PREFIX}_pidfile_unlock(int fd) {
        /* Do not unlink: a new inode would allow concurrent lock owners. */
        return close(fd);
    }
}
