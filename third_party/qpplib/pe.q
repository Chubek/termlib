# pe.q — PE32/PE32+ header and section reader; borrowed input

@module pe
@version 1.0

@param PREFIX pe

@include <stddef.h>
@include <stdint.h>
@include <string.h>

@guard

@import binfile PREFIX=${PREFIX}_bytes

@raw {
    typedef struct {
        ${PREFIX}_bytes_t bytes; size_t sections; uint16_t machine, section_count, characteristics;
        int bits; uint32_t entry_rva, headers_size; uint64_t image_base;
    } ${PREFIX}_t;
    typedef struct { char name[9]; uint32_t virtual_size, rva, raw_size, raw_offset, characteristics; } ${PREFIX}_section;
}

@fn get {
    static inline uint64_t ${PREFIX}_get(const ${PREFIX}_t *p, size_t offset, unsigned width) {
        uint64_t value=0; ${PREFIX}_bytes_uint(&p->bytes,offset,width,0,&value); return value;
    }
}

@fn init {
    static inline int ${PREFIX}_init(${PREFIX}_t *out, const void *data, size_t size) {
        ${PREFIX}_t p; size_t off, opt; uint16_t optsize, magic;
        if (!out || !data || size < 64 || memcmp(data,"MZ",2)) return 0;
        memset(&p,0,sizeof p); ${PREFIX}_bytes_init(&p.bytes,data,size);
        off=(size_t)${PREFIX}_get(&p,60,4);
        if (off < 64 || !${PREFIX}_bytes_range(&p.bytes,off,24) || memcmp(p.bytes.data+off,"PE\\0\\0",4)) return 0;
        p.machine=(uint16_t)${PREFIX}_get(&p,off+4,2); p.section_count=(uint16_t)${PREFIX}_get(&p,off+6,2);
        optsize=(uint16_t)${PREFIX}_get(&p,off+20,2); p.characteristics=(uint16_t)${PREFIX}_get(&p,off+22,2); opt=off+24;
        if (optsize < 2 || !${PREFIX}_bytes_range(&p.bytes,opt,optsize)) return 0;
        magic=(uint16_t)${PREFIX}_get(&p,opt,2);
        if ((magic != 0x10b && magic != 0x20b) || optsize < (magic == 0x10b ? 96 : 112)) return 0;
        p.bits=magic == 0x10b ? 32 : 64;
        p.entry_rva=(uint32_t)${PREFIX}_get(&p,opt+16,4);
        p.image_base=${PREFIX}_get(&p,opt+(p.bits == 32 ? 28 : 24),p.bits/8);
        p.headers_size=(uint32_t)${PREFIX}_get(&p,opt+60,4); p.sections=opt+optsize;
        if (p.section_count > (size-p.sections)/40) return 0;
        if (p.headers_size < p.sections+(size_t)p.section_count*40 || p.headers_size > size) return 0;
        if (${PREFIX}_get(&p,opt+(p.bits == 32 ? 92 : 108),4) > (optsize-(p.bits == 32 ? 96u : 112u))/8) return 0;
        *out=p; return 1;
    }
}

@fn section_at {
    static inline int ${PREFIX}_section_at(const ${PREFIX}_t *p, size_t index, ${PREFIX}_section *out) {
        ${PREFIX}_section s; size_t o;
        if (!p || !out || index >= p->section_count) return 0;
        o=p->sections+index*40; memcpy(s.name,p->bytes.data+o,8); s.name[8]=0;
        s.virtual_size=(uint32_t)${PREFIX}_get(p,o+8,4); s.rva=(uint32_t)${PREFIX}_get(p,o+12,4);
        s.raw_size=(uint32_t)${PREFIX}_get(p,o+16,4); s.raw_offset=(uint32_t)${PREFIX}_get(p,o+20,4);
        s.characteristics=(uint32_t)${PREFIX}_get(p,o+36,4);
        if (s.raw_size && !${PREFIX}_bytes_range(&p->bytes,s.raw_offset,s.raw_size)) return 0;
        *out=s; return 1;
    }
}

@fn rva_offset {
    static inline int ${PREFIX}_rva_offset(const ${PREFIX}_t *p, uint32_t rva, size_t *out) {
        size_t i; ${PREFIX}_section s;
        if (!p || !out) return 0;
        if (rva < p->headers_size) { *out=rva; return 1; }
        for (i=0;i<p->section_count;++i) {
            if (!${PREFIX}_section_at(p,i,&s)) return 0;
            if (rva >= s.rva && (uint64_t)rva-s.rva < s.raw_size) {
                *out=(size_t)s.raw_offset+(rva-s.rva); return 1;
            }
        }
        return 0;
    }
}
