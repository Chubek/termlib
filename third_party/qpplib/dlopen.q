# dlopen.q — POSIX shared object loading; link with -ldl

@module dlopen
@version 1.0

@param PREFIX dlopen

@include <dlfcn.h>
@include <errno.h>
@include <stddef.h>

@guard

@raw {
    typedef struct { void *handle; } ${PREFIX}_t;
}

@fn init {
    static inline void ${PREFIX}_init(${PREFIX}_t *p) { p->handle = NULL; }
}

@fn open {
    static inline int ${PREFIX}_open(${PREFIX}_t *p, const char *path, int flags) {
        if (!p || p->handle) { errno = EINVAL; return 0; }
        p->handle = dlopen(path, flags); return p->handle != NULL;
    }
}

@fn symbol {
    static inline int ${PREFIX}_symbol(const ${PREFIX}_t *p, const char *name, void **out) {
        void *symbol; const char *error;
        if (!p || !p->handle || !name || !out) return 0;
        dlerror(); symbol = dlsym(p->handle, name); error = dlerror();
        if (error) return 0;
        *out = symbol; return 1;
    }
}

@fn error {
    static inline const char *${PREFIX}_error(void) { return dlerror(); }
}

@fn close {
    static inline int ${PREFIX}_close(${PREFIX}_t *p) {
        if (!p || !p->handle) return 0;
        if (dlclose(p->handle)) return 0;
        p->handle = NULL; return 1;
    }
}
