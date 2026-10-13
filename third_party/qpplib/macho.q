# macho.q — thin Mach-O 32/64-bit headers and load commands; no fat containers

@module macho
@version 1.0

@param PREFIX macho

@include <stddef.h>
@include <stdint.h>
@include <string.h>

@guard

@import binfile PREFIX=${PREFIX}_bytes

@raw {
    typedef struct {
        ${PREFIX}_bytes_t bytes; int bits, big; uint32_t cpu, subtype, filetype, ncmds, sizeofcmds, flags;
        size_t header_size;
    } ${PREFIX}_t;
    typedef struct { uint32_t cmd, size; size_t offset; ${PREFIX}_bytes_t bytes; } ${PREFIX}_command;
}

@fn get {
    static inline uint32_t ${PREFIX}_get(const ${PREFIX}_t *p, size_t offset) {
        uint64_t value=0; ${PREFIX}_bytes_uint(&p->bytes,offset,4,p->big,&value); return (uint32_t)value;
    }
}

@fn init {
    static inline int ${PREFIX}_init(${PREFIX}_t *out, const void *data, size_t size) {
        ${PREFIX}_t p; uint64_t magic; size_t off, end; uint32_t i;
        if (!out || !data || size < 28) return 0;
        memset(&p,0,sizeof p); ${PREFIX}_bytes_init(&p.bytes,data,size);
        ${PREFIX}_bytes_uint(&p.bytes,0,4,0,&magic);
        if (magic != 0xfeedface && magic != 0xfeedfacf && magic != 0xcefaedfe && magic != 0xcffaedfe) return 0;
        p.big=magic == 0xcefaedfe || magic == 0xcffaedfe;
        p.bits=(magic == 0xfeedfacf || magic == 0xcffaedfe) ? 64 : 32; p.header_size=p.bits == 64 ? 32 : 28;
        if (size < p.header_size) return 0;
        p.cpu=${PREFIX}_get(&p,4); p.subtype=${PREFIX}_get(&p,8); p.filetype=${PREFIX}_get(&p,12);
        p.ncmds=${PREFIX}_get(&p,16); p.sizeofcmds=${PREFIX}_get(&p,20); p.flags=${PREFIX}_get(&p,24);
        if (!${PREFIX}_bytes_range(&p.bytes,p.header_size,p.sizeofcmds) || p.ncmds > p.sizeofcmds/8) return 0;
        off=p.header_size; end=off+p.sizeofcmds;
        for (i=0;i<p.ncmds;++i) {
            uint32_t n;
            if (end-off < 8) return 0;
            n=${PREFIX}_get(&p,off+4);
            if (n < 8 || n % (p.bits == 64 ? 8 : 4) || n > end-off) return 0;
            off+=n;
        }
        if (off != end) return 0;
        *out=p; return 1;
    }
}

@fn command_at {
    static inline int ${PREFIX}_command_at(const ${PREFIX}_t *p, size_t index, ${PREFIX}_command *out) {
        size_t off, i; ${PREFIX}_command c;
        if (!p || !out || index >= p->ncmds) return 0;
        off=p->header_size;
        for (i=0;i<index;++i) off+=${PREFIX}_get(p,off+4);
        c.cmd=${PREFIX}_get(p,off); c.size=${PREFIX}_get(p,off+4); c.offset=off;
        if (!${PREFIX}_bytes_slice(&p->bytes,off,c.size,&c.bytes)) return 0;
        *out=c; return 1;
    }
}
