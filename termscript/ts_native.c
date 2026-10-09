/* Keystone assembles the complete QBE unit, including local branches and
 * literals. External calls go through five-byte jump veneers carrying ELF
 * PLT32 relocations; this keeps symbol resolution in the system linker. */
#define _GNU_SOURCE
#include <elf.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <keystone/keystone.h>

/* This bundled Keystone defaults to radix 16 even for AT&T syntax.
 * Spell QBE's decimal tokens explicitly, leaving register/label names intact. */
static void
gas_line (FILE *out, const char *p)
{
  const char *start = p;
  while (*p)
    {
      if (isdigit ((unsigned char) *p) &&
          (p == start || (!isalnum ((unsigned char) p[-1]) && p[-1] != '_' && p[-1] != '.')))
        {
          char *end;
          unsigned long long n = strtoull (p, &end, 10);
          fprintf (out, "0x%llx", n); p = end;
        }
      else fputc (*p++, out);
    }
}

static int
write_object (const char *path, unsigned char *code, size_t size,
              char **names, size_t count)
{
  static const char sections[] = "\0.text\0.rela.text\0.symtab\0.strtab\0.shstrtab\0.note.GNU-stack\0";
  Elf64_Ehdr eh = {0};
  Elf64_Shdr sh[7] = {{0}};
  Elf64_Sym *syms = calloc (count + 2, sizeof *syms);
  Elf64_Rela *rels = calloc (count ? count : 1, sizeof *rels);
  char *strings = NULL;
  size_t slen = 0, i, offset;
  FILE *str = open_memstream (&strings, &slen), *out = NULL;
  int rc = 1;
  const uint64_t zero = 0;
  if (!syms || !rels || !str) goto done;
  fputc (0, str); fputs ("ts_qbe_run", str); fputc (0, str);
  syms[1].st_name = 1; syms[1].st_info = ELF64_ST_INFO (STB_GLOBAL, STT_FUNC);
  syms[1].st_shndx = 1;
  for (i = 0; i < count; i++)
    {
      syms[i + 2].st_name = (Elf64_Word) ftell (str);
      syms[i + 2].st_info = ELF64_ST_INFO (STB_GLOBAL, STT_NOTYPE);
      fputs (names[i], str); fputc (0, str);
      rels[i].r_offset = size - count * 5 + i * 5 + 1;
      rels[i].r_info = ELF64_R_INFO (i + 2, R_X86_64_PLT32);
      rels[i].r_addend = -4;
    }
  if (fclose (str)) { str = NULL; goto done; } str = NULL;
  memcpy (eh.e_ident, ELFMAG, SELFMAG);
  eh.e_ident[EI_CLASS] = ELFCLASS64; eh.e_ident[EI_DATA] = ELFDATA2LSB;
  eh.e_ident[EI_VERSION] = EV_CURRENT;
  eh.e_type = ET_REL; eh.e_machine = EM_X86_64; eh.e_version = EV_CURRENT;
  eh.e_ehsize = sizeof eh; eh.e_shentsize = sizeof *sh;
  eh.e_shnum = 7; eh.e_shstrndx = 5;
  offset = sizeof eh;
  sh[1].sh_name = 1; sh[1].sh_type = SHT_PROGBITS;
  sh[1].sh_flags = SHF_ALLOC | SHF_EXECINSTR; sh[1].sh_addralign = 16;
  sh[1].sh_offset = offset; sh[1].sh_size = size; offset += (size + 7) & ~(size_t) 7;
  sh[2].sh_name = 7; sh[2].sh_type = SHT_RELA; sh[2].sh_link = 3; sh[2].sh_info = 1;
  sh[2].sh_addralign = 8; sh[2].sh_entsize = sizeof *rels;
  sh[2].sh_offset = offset; sh[2].sh_size = count * sizeof *rels; offset += sh[2].sh_size;
  sh[3].sh_name = 18; sh[3].sh_type = SHT_SYMTAB; sh[3].sh_link = 4; sh[3].sh_info = 1;
  sh[3].sh_addralign = 8; sh[3].sh_entsize = sizeof *syms;
  sh[3].sh_offset = offset; sh[3].sh_size = (count + 2) * sizeof *syms; offset += sh[3].sh_size;
  sh[4].sh_name = 26; sh[4].sh_type = SHT_STRTAB; sh[4].sh_addralign = 1;
  sh[4].sh_offset = offset; sh[4].sh_size = slen; offset += slen;
  sh[5].sh_name = 34; sh[5].sh_type = SHT_STRTAB; sh[5].sh_addralign = 1;
  sh[5].sh_offset = offset; sh[5].sh_size = sizeof sections; offset += sizeof sections;
  sh[6].sh_name = 44; sh[6].sh_type = SHT_PROGBITS; sh[6].sh_addralign = 1;
  eh.e_shoff = (offset + 7) & ~(size_t) 7;
  out = fopen (path, "wb");
  if (!out) goto done;
#define WRITE(p, n) do { if (fwrite ((p), 1, (n), out) != (n)) goto done; } while (0)
  WRITE (&eh, sizeof eh); WRITE (code, size);
  WRITE (&zero, ((size + 7) & ~(size_t) 7) - size);
  WRITE (rels, count * sizeof *rels); WRITE (syms, (count + 2) * sizeof *syms);
  WRITE (strings, slen); WRITE (sections, sizeof sections);
  WRITE (&zero, eh.e_shoff - offset); WRITE (sh, sizeof sh);
#undef WRITE
  rc = 0;
done:
  if (str) fclose (str);
  if (out && fclose (out)) rc = 1;
  free (syms); free (rels); free (strings);
  if (rc) remove (path);
  return rc;
}

int
ts_native_assemble (const char *input, const char *output)
{
  FILE *in = fopen (input, "r"), *text = NULL, *data = NULL;
  char *line = NULL, *asmtext = NULL, *datatext = NULL;
  char *names[64] = {0};
  size_t cap = 0, len = 0, dlen = 0, n = 0, i;
  unsigned char *bytes = NULL;
  size_t size = 0, statements = 0;
  ks_engine *ks = NULL;
  int rc = 1, in_data = 0;
  if (!in) goto done;
  text = open_memstream (&asmtext, &len);
  data = open_memstream (&datatext, &dlen);
  if (!text || !data) goto done;
  while (getline (&line, &cap, in) >= 0)
    {
      char *p = line;
      while (*p == ' ' || *p == '\t') p++;
      if (!strncmp (p, ".data", 5) || !strncmp (p, ".bss", 4)) { in_data = 1; continue; }
      if (!strncmp (p, ".text", 5)) { in_data = 0; continue; }
      if (!strncmp (p, ".globl", 6) || !strncmp (p, ".type", 5) ||
          !strncmp (p, ".size", 5) || !strncmp (p, ".section .note", 14)) continue;
      if (!strncmp (p, "endbr64", 7)) { fputs (".byte 0xf3,0x0f,0x1e,0xfa\n", text); continue; }
      if (!strncmp (p, "callq ", 6) && p[6] != '*')
        {
          char name[128];
          if (sscanf (p + 6, "%127s", name) != 1) goto done;
          for (i = 0; i < n && strcmp (names[i], name); i++) ;
          if (i == n)
            {
              if (n == 64 || !(names[n] = strdup (name))) goto done;
              n++;
            }
          fprintf (text, "callq ts_external_%zu\n", i);
        }
      else gas_line (in_data ? data : text, p);
    }
  if (ferror (in)) goto done;
  if (fclose (data)) { data = NULL; goto done; } data = NULL;
  fputs (datatext, text);
  for (i = 0; i < n; i++) fprintf (text, "ts_external_%zu:\n.byte 0xe9,0,0,0,0\n", i);
  if (fclose (text)) { text = NULL; goto done; } text = NULL;
  if (ks_open (KS_ARCH_X86, KS_MODE_64, &ks) != KS_ERR_OK) goto done;
  if (ks_option (ks, KS_OPT_SYNTAX, KS_OPT_SYNTAX_ATT) != KS_ERR_OK) goto done;
  if (ks_asm (ks, asmtext, 0, &bytes, &size, &statements))
    { fprintf (stderr, "termscript: Keystone: %s (after %zu statements)\n", ks_strerror (ks_errno (ks)), statements); goto done; }
  if (size < n * 5) goto done;
  rc = write_object (output, bytes, size, names, n);
done:
  if (rc) fprintf (stderr, "termscript: native assembly failed\n");
  if (in) fclose (in);
  if (text) fclose (text);
  if (data) fclose (data);
  if (bytes) ks_free (bytes);
  if (ks) ks_close (ks);
  for (i = 0; i < n; i++) free (names[i]);
  free (line); free (asmtext); free (datatext);
  return rc;
}
