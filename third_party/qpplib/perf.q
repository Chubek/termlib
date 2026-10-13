# perf.q — monotonic stopwatch and online duration statistics

@module perf
@version 1.0

@param PREFIX perf

@include <time.h>
@include <stdint.h>
@include <math.h>

@guard

@raw {
    typedef struct { struct timespec start; int running; } ${PREFIX}_timer;
    typedef struct { uint64_t count; double mean, m2, min, max; } ${PREFIX}_stats;
}

@fn init {
    static inline void ${PREFIX}_init(${PREFIX}_timer *t) { t->running=0; }
}

@fn start {
    static inline int ${PREFIX}_start(${PREFIX}_timer *t) {
        if (clock_gettime(CLOCK_MONOTONIC,&t->start)) return 0;
        t->running=1; return 1;
    }
}

@fn elapsed {
    static inline int ${PREFIX}_elapsed(const ${PREFIX}_timer *t, double *seconds) {
        struct timespec now;
        if (!t->running || !seconds || clock_gettime(CLOCK_MONOTONIC,&now)) return 0;
        *seconds=(double)(now.tv_sec-t->start.tv_sec)+(double)(now.tv_nsec-t->start.tv_nsec)/1e9; return 1;
    }
}

@fn stats_init {
    static inline void ${PREFIX}_stats_init(${PREFIX}_stats *s) { s->count=0; s->mean=s->m2=s->min=s->max=0; }
}

@fn record {
    static inline int ${PREFIX}_record(${PREFIX}_stats *s, double seconds) {
        double delta,mean,m2; uint64_t n;
        if (!isfinite(seconds) || seconds<0 || s->count==UINT64_MAX) return 0;
        n=s->count+1; delta=seconds-s->mean; mean=s->mean+delta/(double)n;
        m2=s->m2+delta*(seconds-mean);
        if (!isfinite(mean) || !isfinite(m2)) return 0;
        if (!s->count || seconds<s->min) s->min=seconds;
        if (!s->count || seconds>s->max) s->max=seconds;
        s->count=n; s->mean=mean; s->m2=m2; return 1;
    }
}

@fn variance {
    static inline double ${PREFIX}_variance(const ${PREFIX}_stats *s) { return s->count>1 ? s->m2/(double)(s->count-1) : 0; }
}
