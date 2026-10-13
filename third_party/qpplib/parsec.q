# parsec.q — byte parser primitives and transactional combinators

@module parsec
@version 1.0

@param PREFIX parsec

@include <stddef.h>
@include <string.h>
@include <stdint.h>

@guard

@raw {
    typedef struct { const unsigned char *data; size_t size, pos; } ${PREFIX}_t;
    typedef int (*${PREFIX}_parser)(${PREFIX}_t *p, void *ctx);
}

@fn init {
    static inline int ${PREFIX}_init(${PREFIX}_t *p, const void *data, size_t size) {
        if (!p || (!data && size)) return 0;
        p->data = (const unsigned char *)data; p->size = size; p->pos = 0; return 1;
    }
}

@fn literal {
    static inline int ${PREFIX}_literal(${PREFIX}_t *p, const void *text, size_t size) {
        if (!p || (!text && size) || p->pos > p->size || size > p->size-p->pos) return 0;
        if (size && memcmp(p->data+p->pos, text, size)) return 0;
        p->pos += size; return 1;
    }
}

@fn byte {
    static inline int ${PREFIX}_byte(${PREFIX}_t *p, unsigned char byte) { return ${PREFIX}_literal(p, &byte, 1); }
}

@fn eof {
    static inline int ${PREFIX}_eof(const ${PREFIX}_t *p) { return p && p->pos == p->size; }
}

@fn uint {
    static inline int ${PREFIX}_uint(${PREFIX}_t *p, uint64_t *out) {
        size_t pos = p->pos, start = pos; uint64_t x = 0;
        if (!out) return 0;
        while (pos < p->size && p->data[pos] >= '0' && p->data[pos] <= '9') {
            unsigned digit = p->data[pos++] - '0';
            if (x > (UINT64_MAX-digit)/10) return 0;
            x = x*10 + digit;
        }
        if (pos == start) return 0;
        p->pos = pos; *out = x; return 1;
    }
}

@fn choice {
    static inline int ${PREFIX}_choice(${PREFIX}_t *p, ${PREFIX}_parser a, void *ac, ${PREFIX}_parser b, void *bc) {
        size_t start = p->pos;
        if (a(p, ac)) return 1;
        p->pos = start;
        if (b(p, bc)) return 1;
        p->pos = start; return 0;
    }
}

@fn sequence {
    static inline int ${PREFIX}_sequence(${PREFIX}_t *p, ${PREFIX}_parser a, void *ac, ${PREFIX}_parser b, void *bc) {
        size_t start = p->pos;
        if (a(p, ac) && b(p, bc)) return 1;
        p->pos = start; return 0;
    }
}

@fn many {
    static inline int ${PREFIX}_many(${PREFIX}_t *p, ${PREFIX}_parser fn, void *ctx, size_t min, size_t max, size_t *out) {
        size_t start = p->pos, n = 0;
        if (min > max) return 0;
        while (n < max) {
            size_t before = p->pos;
            if (!fn(p, ctx)) { p->pos = before; break; }
            if (p->pos <= before || p->pos > p->size) { p->pos = start; return 0; }
            ++n;
        }
        if (n < min) { p->pos = start; return 0; }
        if (out) *out = n;
        return 1;
    }
}
