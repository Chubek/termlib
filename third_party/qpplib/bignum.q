# bignum.q — arbitrary precision unsigned integers with alias-safe arithmetic

@module bignum
@version 1.0

@param PREFIX bignum

@include <stdint.h>
@include <stddef.h>
@include <stdlib.h>
@include <string.h>

@guard

@raw {
    typedef struct { uint32_t *digits; size_t size, cap; } ${PREFIX}_t;
}

@fn init {
    static inline void ${PREFIX}_init(${PREFIX}_t *n) { n->digits=NULL; n->size=n->cap=0; }
}

@fn free {
    static inline void ${PREFIX}_free(${PREFIX}_t *n) { free(n->digits); ${PREFIX}_init(n); }
}

@fn reserve {
    static inline int ${PREFIX}_reserve(${PREFIX}_t *n, size_t cap) {
        uint32_t *d;
        if (cap <= n->cap) return 1;
        if (cap > SIZE_MAX/sizeof *d) return 0;
        d=(uint32_t *)realloc(n->digits,cap*sizeof *d);
        if (!d) return 0;
        n->digits=d; n->cap=cap; return 1;
    }
}

@fn normalize {
    static inline void ${PREFIX}_normalize(${PREFIX}_t *n) { while (n->size && !n->digits[n->size-1]) --n->size; }
}

@fn commit {
    static inline void ${PREFIX}_commit(${PREFIX}_t *out, ${PREFIX}_t *temp) { ${PREFIX}_free(out); *out=*temp; ${PREFIX}_init(temp); }
}

@fn set_u64 {
    static inline int ${PREFIX}_set_u64(${PREFIX}_t *out, uint64_t value) {
        if (!${PREFIX}_reserve(out,2)) return 0;
        out->digits[0]=(uint32_t)value; out->digits[1]=(uint32_t)(value>>32);
        out->size=out->digits[1] ? 2 : (value ? 1 : 0); return 1;
    }
}

@fn copy {
    static inline int ${PREFIX}_copy(${PREFIX}_t *out, const ${PREFIX}_t *n) {
        if (out == n) return 1;
        if (!${PREFIX}_reserve(out,n->size)) return 0;
        if (n->size) memcpy(out->digits,n->digits,n->size*sizeof *n->digits);
        out->size=n->size; return 1;
    }
}

@fn compare {
    static inline int ${PREFIX}_compare(const ${PREFIX}_t *a, const ${PREFIX}_t *b) {
        size_t i;
        if (a->size != b->size) return a->size < b->size ? -1 : 1;
        for (i=a->size;i;--i) if (a->digits[i-1] != b->digits[i-1]) return a->digits[i-1] < b->digits[i-1] ? -1 : 1;
        return 0;
    }
}

@fn add {
    static inline int ${PREFIX}_add(${PREFIX}_t *out, const ${PREFIX}_t *a, const ${PREFIX}_t *b) {
        ${PREFIX}_t t; size_t i,n=a->size > b->size ? a->size : b->size; uint64_t carry=0;
        ${PREFIX}_init(&t);
        if (n == SIZE_MAX || !${PREFIX}_reserve(&t,n+1)) return 0;
        for (i=0;i<n;++i) {
            uint64_t x=carry+(i<a->size ? (uint64_t)a->digits[i] : 0)+(i<b->size ? (uint64_t)b->digits[i] : 0);
            t.digits[i]=(uint32_t)x; carry=x>>32;
        }
        t.digits[n]=(uint32_t)carry; t.size=n+(carry != 0); ${PREFIX}_commit(out,&t); return 1;
    }
}

@fn sub {
    static inline int ${PREFIX}_sub(${PREFIX}_t *out, const ${PREFIX}_t *a, const ${PREFIX}_t *b) {
        ${PREFIX}_t t; size_t i; uint64_t borrow=0;
        if (${PREFIX}_compare(a,b)<0) return 0;
        ${PREFIX}_init(&t); if (!${PREFIX}_reserve(&t,a->size)) return 0;
        for (i=0;i<a->size;++i) {
            uint64_t x=a->digits[i], y=borrow+(i<b->size ? (uint64_t)b->digits[i] : 0);
            t.digits[i]=(uint32_t)(x-y); borrow=x<y;
        }
        t.size=a->size; ${PREFIX}_normalize(&t); ${PREFIX}_commit(out,&t); return 1;
    }
}

@fn mul {
    static inline int ${PREFIX}_mul(${PREFIX}_t *out, const ${PREFIX}_t *a, const ${PREFIX}_t *b) {
        ${PREFIX}_t t; size_t i,j,n;
        ${PREFIX}_init(&t);
        if (!a->size || !b->size) { ${PREFIX}_commit(out,&t); return 1; }
        if (a->size > SIZE_MAX-b->size) return 0;
        n=a->size+b->size; if (!${PREFIX}_reserve(&t,n)) return 0;
        memset(t.digits,0,n*sizeof *t.digits);
        for (i=0;i<a->size;++i) {
            uint64_t carry=0;
            for (j=0;j<b->size;++j) {
                uint64_t x=(uint64_t)a->digits[i]*b->digits[j]+t.digits[i+j]+carry;
                t.digits[i+j]=(uint32_t)x; carry=x>>32;
            }
            t.digits[i+b->size]=(uint32_t)carry;
        }
        t.size=n; ${PREFIX}_normalize(&t); ${PREFIX}_commit(out,&t); return 1;
    }
}

@fn div_u32 {
    static inline int ${PREFIX}_div_u32(${PREFIX}_t *out, const ${PREFIX}_t *a, uint32_t divisor, uint32_t *remainder) {
        ${PREFIX}_t t; size_t i; uint64_t rem=0;
        if (!divisor) return 0;
        ${PREFIX}_init(&t); if (!${PREFIX}_reserve(&t,a->size)) return 0;
        for (i=a->size;i;--i) {
            uint64_t x=(rem<<32)|a->digits[i-1]; t.digits[i-1]=(uint32_t)(x/divisor); rem=x%divisor;
        }
        t.size=a->size; ${PREFIX}_normalize(&t); ${PREFIX}_commit(out,&t);
        if (remainder) *remainder=(uint32_t)rem;
        return 1;
    }
}

@fn from_decimal {
    static inline int ${PREFIX}_from_decimal(${PREFIX}_t *out, const char *s) {
        ${PREFIX}_t t; size_t i;
        if (!s || !*s) return 0;
        ${PREFIX}_init(&t);
        for (;*s;++s) {
            uint64_t carry;
            if (*s<'0' || *s>'9' || t.size == SIZE_MAX || !${PREFIX}_reserve(&t,t.size+1)) { ${PREFIX}_free(&t); return 0; }
            carry=(unsigned)(*s-'0');
            for (i=0;i<t.size;++i) { uint64_t x=(uint64_t)t.digits[i]*10+carry; t.digits[i]=(uint32_t)x; carry=x>>32; }
            if (carry) t.digits[t.size++]=(uint32_t)carry;
        }
        ${PREFIX}_commit(out,&t); return 1;
    }
}

@fn to_decimal {
    static inline int ${PREFIX}_to_decimal(const ${PREFIX}_t *a, char *out, size_t cap) {
        ${PREFIX}_t t; char *tmp; size_t n=0,i,limit;
        if (!out || !cap || a->size > (SIZE_MAX-2)/10) return 0;
        limit=a->size*10+2; tmp=(char *)malloc(limit);
        if (!tmp) return 0;
        ${PREFIX}_init(&t);
        if (!${PREFIX}_copy(&t,a)) { free(tmp); return 0; }
        do {
            uint64_t rem=0;
            for (i=t.size;i;--i) { uint64_t x=(rem<<32)|t.digits[i-1]; t.digits[i-1]=(uint32_t)(x/10); rem=x%10; }
            tmp[n++]=(char)('0'+rem); ${PREFIX}_normalize(&t);
        } while (t.size);
        ${PREFIX}_free(&t);
        if (n >= cap) { free(tmp); return 0; }
        for (i=0;i<n;++i) out[i]=tmp[n-1-i];
        out[n]=0; free(tmp); return 1;
    }
}
