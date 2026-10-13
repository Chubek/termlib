# messaging.q — bounded thread-safe typed mailbox; link with -pthread

@module messaging
@version 1.0

@param PREFIX messaging
@param T
@param CAP 64

@require {$CAP > 0} "CAP must be positive"

@include <pthread.h>
@include <stddef.h>
@include <errno.h>

@guard

@raw {
    typedef struct {
        ${T} data[${CAP}]; size_t head, count; int closed;
        pthread_mutex_t mutex; pthread_cond_t readable, writable;
    } ${PREFIX}_t;
    /* init returns a pthread error code. send/recv: 1 success, 0 closed,
     * -1 would block (nonblocking mode). No operations after destroy. */
}

@fn init {
    static inline int ${PREFIX}_init(${PREFIX}_t *q) {
        int rc; q->head=q->count=0; q->closed=0;
        rc=pthread_mutex_init(&q->mutex,NULL); if (rc) return rc;
        rc=pthread_cond_init(&q->readable,NULL);
        if (rc) { pthread_mutex_destroy(&q->mutex); return rc; }
        rc=pthread_cond_init(&q->writable,NULL);
        if (rc) { pthread_cond_destroy(&q->readable); pthread_mutex_destroy(&q->mutex); }
        return rc;
    }
}

@fn send {
    static inline int ${PREFIX}_send(${PREFIX}_t *q, ${T} value, int wait) {
        pthread_mutex_lock(&q->mutex);
        while (q->count == ${CAP} && !q->closed) {
            if (!wait) { pthread_mutex_unlock(&q->mutex); return -1; }
            pthread_cond_wait(&q->writable,&q->mutex);
        }
        if (q->closed) { pthread_mutex_unlock(&q->mutex); return 0; }
        q->data[(q->head+q->count)%${CAP}]=value; ++q->count;
        pthread_cond_signal(&q->readable); pthread_mutex_unlock(&q->mutex); return 1;
    }
}

@fn recv {
    static inline int ${PREFIX}_recv(${PREFIX}_t *q, ${T} *out, int wait) {
        if (!out) { errno=EINVAL; return -1; }
        pthread_mutex_lock(&q->mutex);
        while (!q->count && !q->closed) {
            if (!wait) { pthread_mutex_unlock(&q->mutex); return -1; }
            pthread_cond_wait(&q->readable,&q->mutex);
        }
        if (!q->count) { pthread_mutex_unlock(&q->mutex); return 0; }
        *out=q->data[q->head]; q->head=(q->head+1)%${CAP}; --q->count;
        pthread_cond_signal(&q->writable); pthread_mutex_unlock(&q->mutex); return 1;
    }
}

@fn close {
    static inline void ${PREFIX}_close(${PREFIX}_t *q) {
        pthread_mutex_lock(&q->mutex); q->closed=1;
        pthread_cond_broadcast(&q->readable); pthread_cond_broadcast(&q->writable);
        pthread_mutex_unlock(&q->mutex);
    }
}

@fn destroy {
    static inline void ${PREFIX}_destroy(${PREFIX}_t *q) {
        /* Caller must close and join every producer/consumer first. */
        pthread_cond_destroy(&q->readable); pthread_cond_destroy(&q->writable); pthread_mutex_destroy(&q->mutex);
    }
}
