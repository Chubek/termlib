# elf.q — ELF32/ELF64 headers, program headers, and sections; borrowed input

@module elf
@version 1.0

@param PREFIX elf

@include <stddef.h>
@include <stdint.h>
@include <string.h>

@guard

@import binfile PREFIX=${PREFIX}_bytes

@raw {
    typedef struct {
        ${PREFIX}_bytes_t bytes; int bits, big; uint16_t type, machine;
        uint64_t entry, phoff, shoff; uint16_t phnum, shnum, phentsize, shentsize, shstrndx;
    } ${PREFIX}_t;
    typedef struct { uint32_t type, flags; uint64_t offset, vaddr, filesz, memsz, align; } ${PREFIX}_segment;
    typedef struct { uint32_t name, type, link, info; uint64_t flags, addr, offset, size, align, entsize; } ${PREFIX}_section;
}

@fn get {
    static inline uint64_t ${PREFIX}_get(const ${PREFIX}_t *p, size_t offset, unsigned width) {
        uint64_t value = 0; ${PREFIX}_bytes_uint(&p->bytes, offset, width, p->big, &value); return value;
    }
}

@fn table {
    static inline int ${PREFIX}_table(const ${PREFIX}_t *p, uint64_t off, uint64_t count, uint64_t stride) {
        return off <= p->bytes.size && (!count || (stride && count <= (p->bytes.size-off)/stride));
    }
}

@fn init {
    static inline int ${PREFIX}_init(${PREFIX}_t *out, const void *data, size_t size) {
        ${PREFIX}_t p; const unsigned char *b = (const unsigned char *)data; unsigned h;
        if (!out || !data || size < 16 || memcmp(b, "\\177ELF", 4) ||
            (b[4] != 1 && b[4] != 2) || (b[5] != 1 && b[5] != 2) || b[6] != 1) return 0;
        memset(&p, 0, sizeof p); ${PREFIX}_bytes_init(&p.bytes, data, size);
        p.bits = b[4] == 1 ? 32 : 64; p.big = b[5] == 2; h = p.bits == 32 ? 52 : 64;
        if (size < h || ${PREFIX}_get(&p, 20, 4) != 1) return 0;
        p.type = (uint16_t)${PREFIX}_get(&p, 16, 2); p.machine = (uint16_t)${PREFIX}_get(&p, 18, 2);
        p.entry = ${PREFIX}_get(&p, 24, p.bits/8);
        p.phoff = ${PREFIX}_get(&p, p.bits == 32 ? 28 : 32, p.bits/8);
        p.shoff = ${PREFIX}_get(&p, p.bits == 32 ? 32 : 40, p.bits/8);
        if (${PREFIX}_get(&p, h-12, 2) != h) return 0;
        p.phentsize = (uint16_t)${PREFIX}_get(&p, h-10, 2); p.phnum = (uint16_t)${PREFIX}_get(&p, h-8, 2);
        p.shentsize = (uint16_t)${PREFIX}_get(&p, h-6, 2); p.shnum = (uint16_t)${PREFIX}_get(&p, h-4, 2);
        p.shstrndx = (uint16_t)${PREFIX}_get(&p, h-2, 2);
        /* Extended numbering is deliberately rejected, never silently truncated. */
        if (p.phnum == 65535 || p.shstrndx >= 0xff00 || p.shnum >= 0xff00 || (!p.shnum && p.shoff)) return 0;
        if (p.shstrndx && p.shstrndx >= p.shnum) return 0;
        if ((p.phnum && (!p.phoff || p.phentsize < (p.bits == 32 ? 32 : 56))) ||
            (p.shnum && (!p.shoff || p.shentsize < (p.bits == 32 ? 40 : 64))) ||
            !${PREFIX}_table(&p, p.phoff, p.phnum, p.phentsize) || !${PREFIX}_table(&p, p.shoff, p.shnum, p.shentsize)) return 0;
        *out = p; return 1;
    }
}

@fn segment_at {
    static inline int ${PREFIX}_segment_at(const ${PREFIX}_t *p, size_t index, ${PREFIX}_segment *out) {
        ${PREFIX}_segment s; size_t o;
        if (!p || !out || index >= p->phnum) return 0;
        o = (size_t)p->phoff + index*p->phentsize;
        s.type = (uint32_t)${PREFIX}_get(p, o, 4);
        if (p->bits == 32) {
            s.offset=${PREFIX}_get(p,o+4,4); s.vaddr=${PREFIX}_get(p,o+8,4);
            s.filesz=${PREFIX}_get(p,o+16,4); s.memsz=${PREFIX}_get(p,o+20,4);
            s.flags=(uint32_t)${PREFIX}_get(p,o+24,4); s.align=${PREFIX}_get(p,o+28,4);
        } else {
            s.flags=(uint32_t)${PREFIX}_get(p,o+4,4); s.offset=${PREFIX}_get(p,o+8,8); s.vaddr=${PREFIX}_get(p,o+16,8);
            s.filesz=${PREFIX}_get(p,o+32,8); s.memsz=${PREFIX}_get(p,o+40,8); s.align=${PREFIX}_get(p,o+48,8);
        }
        if (!${PREFIX}_table(p, s.offset, s.filesz, 1) || (s.type == 1 && s.filesz > s.memsz)) return 0;
        *out = s; return 1;
    }
}

@fn section_at {
    static inline int ${PREFIX}_section_at(const ${PREFIX}_t *p, size_t index, ${PREFIX}_section *out) {
        ${PREFIX}_section s; size_t o; unsigned w;
        if (!p || !out || index >= p->shnum) return 0;
        o=(size_t)p->shoff+index*p->shentsize; w=(unsigned)p->bits/8;
        s.name=(uint32_t)${PREFIX}_get(p,o,4); s.type=(uint32_t)${PREFIX}_get(p,o+4,4);
        s.flags=${PREFIX}_get(p,o+8,w); s.addr=${PREFIX}_get(p,o+8+w,w);
        s.offset=${PREFIX}_get(p,o+8+2*w,w); s.size=${PREFIX}_get(p,o+8+3*w,w);
        s.link=(uint32_t)${PREFIX}_get(p,o+8+4*w,4); s.info=(uint32_t)${PREFIX}_get(p,o+12+4*w,4);
        s.align=${PREFIX}_get(p,o+16+4*w,w); s.entsize=${PREFIX}_get(p,o+16+5*w,w);
        if (s.type != 8 && !${PREFIX}_table(p,s.offset,s.size,1)) return 0;
        *out=s; return 1;
    }
}
