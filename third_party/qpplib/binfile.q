# binfile.q — bounded borrowed byte views and endian reads

@module binfile
@version 1.0

@param PREFIX binfile

@include <stddef.h>
@include <stdint.h>
@include <string.h>

@guard

@raw {
    typedef struct { const unsigned char *data; size_t size; } ${PREFIX}_t;
}

@fn init {
    static inline int ${PREFIX}_init(${PREFIX}_t *v, const void *data, size_t size) {
        if (!v || (!data && size)) return 0;
        v->data = (const unsigned char *)data; v->size = size; return 1;
    }
}

@fn range {
    static inline int ${PREFIX}_range(const ${PREFIX}_t *v, size_t offset, size_t size) {
        return v && (v->data || !v->size) && offset <= v->size && size <= v->size - offset;
    }
}

@fn slice {
    static inline int ${PREFIX}_slice(const ${PREFIX}_t *v, size_t offset, size_t size, ${PREFIX}_t *out) {
        if (!out || !${PREFIX}_range(v, offset, size)) return 0;
        out->data = v->data ? v->data + offset : NULL; out->size = size; return 1;
    }
}

@fn read {
    static inline int ${PREFIX}_read(const ${PREFIX}_t *v, size_t offset, void *out, size_t size) {
        if ((!out && size) || !${PREFIX}_range(v, offset, size)) return 0;
        if (size) memcpy(out, v->data + offset, size);
        return 1;
    }
}

@fn uint {
    static inline int ${PREFIX}_uint(const ${PREFIX}_t *v, size_t offset, unsigned width, int big, uint64_t *out) {
        uint64_t x = 0; unsigned i;
        if (!out || !width || width > 8 || !${PREFIX}_range(v, offset, width)) return 0;
        for (i = 0; i < width; ++i)
            x = (x << 8) | v->data[offset + (big ? i : width - 1 - i)];
        *out = x; return 1;
    }
}
