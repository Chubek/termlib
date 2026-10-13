# schema.q — typed flat-record validation with structured errors

@module schema
@version 1.0

@param PREFIX schema

@include <stddef.h>
@include <string.h>
@include <math.h>

@guard

@raw {
    typedef enum { ${PREFIX}_BOOL, ${PREFIX}_INT, ${PREFIX}_NUMBER, ${PREFIX}_STRING } ${PREFIX}_kind;
    typedef struct {
        const char *name; ${PREFIX}_kind kind;
        union { int boolean; long long integer; double number; const char *string; } as;
    } ${PREFIX}_value;
    typedef struct {
        const char *name; ${PREFIX}_kind kind; int required, bounded;
        double min, max; size_t max_length; /* zero length limit means unlimited */
    } ${PREFIX}_field;
    typedef enum { ${PREFIX}_OK, ${PREFIX}_MISSING, ${PREFIX}_TYPE, ${PREFIX}_RANGE, ${PREFIX}_UNKNOWN, ${PREFIX}_DUPLICATE, ${PREFIX}_INVALID } ${PREFIX}_code;
    typedef struct { ${PREFIX}_code code; size_t field, value; } ${PREFIX}_error;
}

@fn fail {
    static inline int ${PREFIX}_fail(${PREFIX}_error *error, ${PREFIX}_code code, size_t field, size_t value) {
        if (error) { error->code = code; error->field = field; error->value = value; }
        return code == ${PREFIX}_OK;
    }
}

@fn validate {
    static inline int ${PREFIX}_validate(const ${PREFIX}_field *fields, size_t nf, const ${PREFIX}_value *values, size_t nv, int strict, ${PREFIX}_error *error) {
        size_t i, j, k; const size_t none = (size_t)-1;
        if ((!fields && nf) || (!values && nv)) return ${PREFIX}_fail(error, ${PREFIX}_INVALID, none, none);
        for (i = 0; i < nf; ++i) {
            const ${PREFIX}_field *f = fields+i;
            if (!f->name || f->kind < ${PREFIX}_BOOL || f->kind > ${PREFIX}_STRING ||
                (f->bounded && (!isfinite(f->min) || !isfinite(f->max) || f->min > f->max)))
                return ${PREFIX}_fail(error, ${PREFIX}_INVALID, i, none);
            for (k = 0; k < i; ++k) if (!strcmp(f->name, fields[k].name))
                return ${PREFIX}_fail(error, ${PREFIX}_INVALID, i, none);
        }
        for (j = 0; j < nv; ++j) {
            if (!values[j].name) return ${PREFIX}_fail(error, ${PREFIX}_INVALID, none, j);
            for (k = 0; k < j; ++k) if (!strcmp(values[j].name, values[k].name))
                return ${PREFIX}_fail(error, ${PREFIX}_DUPLICATE, none, j);
            for (i = 0; i < nf; ++i) if (!strcmp(values[j].name, fields[i].name)) break;
            if (i == nf && strict) return ${PREFIX}_fail(error, ${PREFIX}_UNKNOWN, none, j);
        }
        for (i = 0; i < nf; ++i) {
            const ${PREFIX}_field *f = fields+i; const ${PREFIX}_value *v; long double x = 0;
            for (j = 0; j < nv; ++j) if (!strcmp(f->name, values[j].name)) break;
            if (j == nv) {
                if (f->required) return ${PREFIX}_fail(error, ${PREFIX}_MISSING, i, none);
                continue;
            }
            v = values+j;
            if (v->kind != f->kind) return ${PREFIX}_fail(error, ${PREFIX}_TYPE, i, j);
            if (f->kind == ${PREFIX}_STRING) {
                if (!v->as.string) return ${PREFIX}_fail(error, ${PREFIX}_TYPE, i, j);
                if (f->max_length && strlen(v->as.string) > f->max_length) return ${PREFIX}_fail(error, ${PREFIX}_RANGE, i, j);
                continue;
            }
            if (f->kind == ${PREFIX}_BOOL) {
                if (v->as.boolean != 0 && v->as.boolean != 1) return ${PREFIX}_fail(error, ${PREFIX}_RANGE, i, j);
                x = v->as.boolean;
            } else if (f->kind == ${PREFIX}_INT) x = v->as.integer;
            else { x = v->as.number; if (!isfinite(v->as.number)) return ${PREFIX}_fail(error, ${PREFIX}_RANGE, i, j); }
            if (f->bounded && (x < (long double)f->min || x > (long double)f->max)) return ${PREFIX}_fail(error, ${PREFIX}_RANGE, i, j);
        }
        return ${PREFIX}_fail(error, ${PREFIX}_OK, none, none);
    }
}
