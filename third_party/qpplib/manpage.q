# manpage.q — roff manpage output with escaped text and macro arguments

@module manpage
@version 1.0

@param PREFIX manpage

@include <stdio.h>

@guard

@raw {
    /* Text is emitted as data: control characters are rejected, roff escapes
     * and leading request characters are neutralized. Stream errors return 0. */
}

@fn text {
    static inline int ${PREFIX}_text(FILE *out, const char *text) {
        int start=1; const unsigned char *p=(const unsigned char *)text;
        if (!out || !text) return 0;
        for (;*p;++p) {
            if ((*p<32 && *p!='\\n' && *p!='\\t') || *p==127) return 0;
            if (start && (*p=='.' || *p=='\\'')) if (fputs("\\\\&",out)==EOF) return 0;
            if (*p=='\\\\') { if (fputs("\\\\e",out)==EOF) return 0; }
            else if (fputc(*p,out)==EOF) return 0;
            start=*p=='\\n';
        }
        return !ferror(out);
    }
}

@fn argument {
    static inline int ${PREFIX}_argument(FILE *out, const char *text) {
        const unsigned char *p=(const unsigned char *)text;
        if (!out || !text) return 0;
        if (fputc('"',out)==EOF) return 0;
        for (;*p;++p) {
            if (*p<32 || *p==127) return 0;
            if (*p=='\\\\') { if (fputs("\\\\e",out)==EOF) return 0; }
            else if (*p=='"') { if (fputs("\\\\(dq",out)==EOF) return 0; }
            else if (fputc(*p,out)==EOF) return 0;
        }
        return fputc('"',out)!=EOF;
    }
}

@fn header {
    static inline int ${PREFIX}_header(FILE *out, const char *title, const char *section, const char *date) {
        if (!out || fputs(".TH ",out)==EOF || !${PREFIX}_argument(out,title) || fputc(' ',out)==EOF ||
            !${PREFIX}_argument(out,section) || fputc(' ',out)==EOF || !${PREFIX}_argument(out,date)) return 0;
        return fputc('\\n',out)!=EOF;
    }
}

@fn section {
    static inline int ${PREFIX}_section(FILE *out, const char *name) {
        return out && fputs(".SH ",out)!=EOF && ${PREFIX}_argument(out,name) && fputc('\\n',out)!=EOF;
    }
}

@fn paragraph {
    static inline int ${PREFIX}_paragraph(FILE *out, const char *text) {
        return out && fputs(".PP\\n",out)!=EOF && ${PREFIX}_text(out,text) && fputc('\\n',out)!=EOF;
    }
}
