/* moosedog_main.c -- Moosedog frontend command line. */
#include "../parser/moosedog_parser.h"
#include "../entrypoint/moosedog_entrypoint.h"
#include "../emitter/moosedog_emitter.h"
#include "../lexer/moosedog_lexer.h"
#include "../rewrite/moosedog_rewrite.h"
#include "../disamb/moosedog_disamb.h"
#include "../lua/moosedog_lua.h"
#include "../plugin/moosedog_plugin.h"
#include "../stdplugin/ebnf-frontend/EBNF-Frontend.h"
#include "../confuse/moosedog_confuse.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

static void
usage (const char *prog)
{
  fprintf (stderr,
           "Usage: %s [options] <grammar.grm|grammar.ebnf>\n"
           "Generate a libglr-backed parser from a Moosedog specification.\n"
           "\n"
           "Options:\n"
           "  -o, --output-dir DIR     output directory (default: .)\n"
           "  --check                  parse and validate only, emit nothing\n"
           "  --emit-makefile          also emit Makefile.<Base>\n"
           "  --emit-cmakefile         also emit CMakeLists.<Base>.txt\n"
           "  --debug                  enable debug annotations\n"
           "  -R, --no-rewrite         skip rewrite passes\n"
           "  -e, --max-errors N       override Config.MaxErrors\n"
           "  -X, --no-cache           disable parse-table cache\n"
           "  -T, --cache-dir DIR      override Config.CacheDirectory\n"
           "  --strict-unicode / --no-strict-unicode\n"
           "  --no-atn                 disable adaptive lookahead\n"
           "  --lookahead-depth N      ATN window 1..64\n"
           "  -D, --define SYM[=VAL]   GPP-style define (repeatable)\n"
           "  -H, --alt-preprocessor F alternate GPP prelude path\n"
           "  --plugin PATH            load a frontend plugin (.so)\n"
           "  --basename NAME          output stem (default: file/grammar name)\n"
           "  --moosedog-include PATH  Moosedog.h source to copy\n"
           "  --version                print version\n"
           "  -h, --help               this help\n"
           "  -v, --verbose            verbose progress\n",
           prog);
}

/* Try GPP preprocessing when defines/includes were requested and gpp
 * exists. When neither is given but a sibling Support.gpp sits next to
 * the grammar (the layout both bundled examples use), preprocess with
 * `gpp -C --include <sibling>` automatically so GPP macros such as
 * ANSI_C_GRAMMAR_NAME resolve. Returns a malloc'd path to the
 * preprocessed file, or NULL to use the original. */
/* Append one shell-quoted word ('...', '\''-escaped) to cmd.
 * Returns 0 on success, -1 when cmd would overflow. */
static int
sh_word (char *cmd, size_t cmd_size, size_t *k, const char *word)
{
  size_t n = *k;
  if (n + 2 >= cmd_size)
    return -1;
  cmd[n++] = '\'';
  for (const char *p = word; *p; p++)
    {
      if (*p == '\'')
        {
          if (n + 4 >= cmd_size)
            return -1;
          cmd[n++] = '\'';
          cmd[n++] = '\\';
          cmd[n++] = '\'';
          cmd[n++] = '\'';
        }
      else
        {
          if (n + 1 >= cmd_size)
            return -1;
          cmd[n++] = *p;
        }
    }
  if (n + 2 >= cmd_size)
    return -1;
  cmd[n++] = '\'';
  cmd[n++] = ' ';
  cmd[n] = '\0';
  *k = n;
  return 0;
}

static char *
maybe_preprocess (const char *grm, char **defines, size_t ndef,
                  const char *alt_preproc, int verbose)
{
  char cmd[8192];
  size_t k;
  char sibling[4096];
  const char *include = alt_preproc;
  if (ndef == 0 && !alt_preproc)
    {
      /* Auto-detect sibling Support.gpp. */
      const char *slash = strrchr (grm, '/');
      int need = slash ? snprintf (NULL, 0, "%.*s/Support.gpp",
                                   (int) (slash - grm), grm)
                       : snprintf (NULL, 0, "Support.gpp");
      if (need < 0 || need >= (int) sizeof (sibling))
        return NULL;
      if (slash)
        snprintf (sibling, sizeof (sibling), "%.*s/Support.gpp",
                  (int) (slash - grm), grm);
      else
        snprintf (sibling, sizeof (sibling), "Support.gpp");
      if (access (sibling, R_OK) == 0)
        include = sibling;
      else
        return NULL;
    }
  if (access ("/usr/bin/gpp", X_OK) != 0 && access ("/usr/local/bin/gpp", X_OK) != 0
      && system ("command -v gpp >/dev/null 2>&1") != 0)
    {
      if (verbose)
        fprintf (stderr, "moosedog: gpp not found; parsing raw input\n");
      return NULL;
    }
  k = snprintf (cmd, sizeof (cmd), "gpp -C ");
  for (size_t i = 0; i < ndef; i++)
    {
      /* One shell word per flag ('-DSYM=VAL'): quoting keeps spaces and
       * metacharacters in values intact. */
      char both[1024];
      if (snprintf (both, sizeof (both), "-D%s", defines[i])
            >= (int) sizeof (both)
          || sh_word (cmd, sizeof (cmd), &k, both) != 0)
        {
          fprintf (stderr, "moosedog: define too long\n");
          return NULL;
        }
    }
  if (include)
    {
      if (sh_word (cmd, sizeof (cmd), &k, "--include") != 0
          || sh_word (cmd, sizeof (cmd), &k, include) != 0)
        return NULL;
    }
  {
    char *tmp = strdup ("/tmp/moosedog-XXXXXX.grm");
    int fd;
    if (!tmp)
      return NULL;
    fd = mkstemps (tmp, 4);
    if (fd < 0)
      {
        free (tmp);
        return NULL;
      }
    close (fd);
    if (sh_word (cmd, sizeof (cmd), &k, grm) != 0
        || sh_word (cmd, sizeof (cmd), &k, "-o") != 0
        || sh_word (cmd, sizeof (cmd), &k, tmp) != 0)
      {
        fprintf (stderr, "moosedog: command too long\n");
        unlink (tmp);
        free (tmp);
        return NULL;
      }
    if (verbose)
      fprintf (stderr, "moosedog: %s\n", cmd);
    if (system (cmd) != 0)
      {
        fprintf (stderr, "moosedog: gpp failed; parsing raw input\n");
        free (tmp);
        return NULL;
      }
    return tmp;
  }
}

static int
has_suffix (const char *path, const char *suf)
{
  size_t a = strlen (path), b = strlen (suf);
  return a >= b && !strcmp (path + a - b, suf);
}

static char *
read_file (const char *path, size_t *len_out)
{
  FILE *f = fopen (path, "rb");
  char *buf;
  size_t len = 0, cap = 1 << 16;
  if (!f)
    return NULL;
  buf = malloc (cap + 1);
  if (!buf)
    {
      fclose (f);
      return NULL;
    }
  for (;;)
    {
      size_t n;
      if (len + 8192 > cap)
        {
          cap *= 2;
          {
            char *nb = realloc (buf, cap + 1);
            if (!nb)
              {
                free (buf);
                fclose (f);
                return NULL;
              }
            buf = nb;
          }
        }
      n = fread (buf + len, 1, cap - len, f);
      len += n;
      if (n == 0)
        break;
    }
  fclose (f);
  buf[len] = '\0';
  if (len_out)
    *len_out = len;
  return buf;
}

int
main (int argc, char **argv)
{
  const char *output_dir = ".";
  const char *stem = NULL;
  const char *moosedog_include = NULL;
  const char *alt_preproc = NULL;
  int check_only = 0, verbose = 0;
  int emit_makefile = 0, emit_cmakefile = 0, debug = 0, no_rewrite = 0;
  long max_errors = -1, lookahead = -1;
  int use_cache = -1, strict_unicode = -1, no_atn = 0;
  char *defines[64];
  size_t ndef = 0;
  char *plugin_paths[16];
  size_t nplug = 0;
  const char *grm_path = NULL;
  int opt;
  static struct option longs[] = {
    { "output-dir", required_argument, NULL, 'o' },
    { "check", no_argument, NULL, 1000 },
    { "emit-makefile", no_argument, NULL, 1001 },
    { "emit-cmakefile", no_argument, NULL, 1002 },
    { "debug", no_argument, NULL, 'd' },
    { "no-rewrite", no_argument, NULL, 'R' },
    { "max-errors", required_argument, NULL, 'e' },
    { "no-cache", no_argument, NULL, 'X' },
    { "cache-dir", required_argument, NULL, 'T' },
    { "strict-unicode", no_argument, NULL, 1003 },
    { "no-strict-unicode", no_argument, NULL, 1004 },
    { "no-atn", no_argument, NULL, 1005 },
    { "lookahead-depth", required_argument, NULL, 1006 },
    { "define", required_argument, NULL, 'D' },
    { "alt-preprocessor", required_argument, NULL, 'H' },
    { "plugin", required_argument, NULL, 1007 },
    { "basename", required_argument, NULL, 1008 },
    { "moosedog-include", required_argument, NULL, 1009 },
    { "version", no_argument, NULL, 1010 },
    { "help", no_argument, NULL, 'h' },
    { "verbose", no_argument, NULL, 'v' },
    { NULL, 0, NULL, 0 }
  };
  md_spec_t spec;
  md_error_t err;
  md_build_config_t cfg;
  md_emit_options_t eopt;
  md_plugin_registry_t preg;
  char *preprocessed = NULL;
  const char *input_to_parse = NULL;
  int rc = 1;

  memset (&spec, 0, sizeof (spec));
  md_plugins_init (&preg);
  while ((opt = getopt_long (argc, argv, "o:dRe:XT:D:H:hvRMCT:", longs, NULL)) != -1)
    {
      switch (opt)
        {
        case 'o':
          output_dir = optarg;
          break;
        case 1000:
          check_only = 1;
          break;
        case 1001:
          emit_makefile = 1;
          break;
        case 1002:
          emit_cmakefile = 1;
          break;
        case 'd':
          debug = 1;
          break;
        case 'R':
          no_rewrite = 1;
          break;
        case 'e':
          max_errors = strtol (optarg, NULL, 10);
          break;
        case 'X':
          use_cache = 0;
          break;
        case 'T':
          /* handled below via cache_dir */
          break;
        case 1003:
          strict_unicode = 1;
          break;
        case 1004:
          strict_unicode = 0;
          break;
        case 1005:
          no_atn = 1;
          break;
        case 1006:
          lookahead = strtol (optarg, NULL, 10);
          break;
        case 'D':
          if (ndef < sizeof (defines) / sizeof (defines[0]))
            defines[ndef++] = optarg;
          break;
        case 'H':
          alt_preproc = optarg;
          break;
        case 1007:
          if (nplug < sizeof (plugin_paths) / sizeof (plugin_paths[0]))
            plugin_paths[nplug++] = optarg;
          break;
        case 1008:
          stem = optarg;
          break;
        case 1009:
          moosedog_include = optarg;
          break;
        case 1010:
          printf ("moosedog 1.0.0 (libglr frontend)\n");
          return 0;
        case 'h':
          usage (argv[0]);
          return 0;
        case 'v':
          verbose = 1;
          break;
        case 'M':
          emit_makefile = 1;
          break;
        case 'C':
          emit_cmakefile = 1;
          break;
        default:
          usage (argv[0]);
          return 2;
        }
    }
  /* --cache-dir arrives as -T due to short-option merger above. */
  for (int i = 1; i < argc; i++)
    if ((!strcmp (argv[i], "--cache-dir") || !strcmp (argv[i], "-T")) && i + 1 < argc)
      {
        /* last wins */
        use_cache = use_cache; /* keep */
      }
  if (optind >= argc)
    {
      usage (argv[0]);
      return 2;
    }
  grm_path = argv[optind];
  /* Load plugins first so .ebnf inputs can use them. */
  for (size_t i = 0; i < nplug; i++)
    {
      char perr[512] = { 0 };
      if (md_plugin_load (&preg, plugin_paths[i], perr, sizeof (perr)) != 0)
        {
          fprintf (stderr, "moosedog: %s\n", perr);
          goto done;
        }
    }
  /* EBNF inputs translate to .grm text first (built-in translator). */
  if (has_suffix (grm_path, ".ebnf"))
    {
      size_t L = 0;
      char *src = read_file (grm_path, &L);
      char *grm = NULL;
      char perr[512] = { 0 };
      int trc = -1;
      if (!src)
        {
          fprintf (stderr, "moosedog: cannot read %s\n", grm_path);
          goto done;
        }
      /* Prefer a loaded plugin for .ebnf, fall back to built-in. */
      if (preg.count > 0)
        trc = md_plugin_translate (&preg, ".ebnf", src, L, &grm, perr,
                                   sizeof (perr));
      if (trc != 0)
        trc = ebnf_to_grm (src, L, "EBNF", &grm, perr, sizeof (perr));
      free (src);
      if (trc != 0)
        {
          fprintf (stderr, "moosedog: ebnf translate failed: %s\n", perr);
          goto done;
        }
      if (md_parse_string (grm, strlen (grm), grm_path, &spec, &err) != 0)
        {
          fprintf (stderr, "moosedog: %s\n", err.message);
          free (grm);
          goto done;
        }
      free (grm);
    }
  else
    {
      preprocessed = maybe_preprocess (grm_path, defines, ndef, alt_preproc,
                                       verbose);
      input_to_parse = preprocessed ? preprocessed : grm_path;
      if (md_parse_file (input_to_parse, &spec, &err) != 0)
        {
          fprintf (stderr, "moosedog: %s\n", err.message);
          goto done;
        }
    }
  /* CLI overrides for per-grammar Optparse parity: honor the flags the
   * JSON/ANSI-C examples declare even though our getopt is generic. */
  if (md_spec_validate (&spec, &err) != 0
      || md_lexer_validate (&spec, &err) != 0
      || md_entrypoint_validate (&spec, &err) != 0
      || md_disamb_validate (&spec, &err) != 0
      || md_rewrite_validate (&spec, &err) != 0)
    {
      fprintf (stderr, "moosedog: %s\n", err.message);
      goto done;
    }
  {
    char basedir[PATH_MAX];
    snprintf (basedir, sizeof (basedir), "%s", grm_path);
    {
      char *slash = strrchr (basedir, '/');
      if (slash)
        *slash = '\0';
      else
        snprintf (basedir, sizeof (basedir), ".");
    }
    if (md_lua_validate (&spec, basedir, &err) != 0)
      {
        fprintf (stderr, "moosedog: %s\n", err.message);
        goto done;
      }
  }
  md_entrypoint_resolve (&spec, &cfg);
  if (no_atn)
    {
      cfg.use_atn = 0;
      cfg.adaptive_lookahead = 0;
    }
  if (lookahead > 0)
    cfg.lookahead_depth = (int) lookahead;
  if (max_errors >= 0)
    cfg.max_errors = (int) max_errors;
  if (use_cache == 0)
    cfg.use_cache = 0;
  if (strict_unicode >= 0)
    cfg.strict_unicode = strict_unicode;
  if (debug)
    cfg.debug_level = 1;
  /* CLI overrides bypass the spec text, so re-apply the entrypoint
   * limits here instead of trusting the flag values. */
  if (cfg.lookahead_depth < 1 || cfg.lookahead_depth > 64)
    {
      fprintf (stderr, "moosedog: --lookahead-depth %d out of range 1..64\n",
               cfg.lookahead_depth);
      goto done;
    }
  if (cfg.max_errors < 0 || cfg.max_errors > 10000)
    {
      fprintf (stderr, "moosedog: --max-errors %d out of range 0..10000\n",
               cfg.max_errors);
      goto done;
    }
  {
    char cache_dir[4096];
    /* Re-scan argv for --cache-dir/-T value (getopt shortcut above). */
    const char *cdir = NULL;
    for (int i = 1; i + 1 < argc; i++)
      if (!strcmp (argv[i], "--cache-dir") || !strcmp (argv[i], "-T"))
        cdir = argv[i + 1];
    if (cdir)
      cfg.cache_dir = cdir;
    if (md_conf_resolve_dir (cfg.cache_dir, cache_dir, sizeof (cache_dir)) != 0)
      {
        fprintf (stderr, "moosedog: bad cache directory\n");
        goto done;
      }
    if (verbose)
      {
        printf ("grammar : %s\n",
                spec.entry.lang_name ? spec.entry.lang_name : "?");
        printf ("rules   : %zu  tokens: %zu (%zu match, %zu skip)  ast nodes: %zu\n",
                spec.rule_count, spec.token_count,
                md_lexer_token_count (&spec), md_lexer_skip_count (&spec),
                spec.node_count);
        printf ("start   : %s\n", spec.start_rule ? spec.start_rule : "?");
        printf ("lexer   : %s / %s\n", cfg.main_lexer, cfg.fallback_lexer);
        printf ("atn     : %s depth %d\n",
                cfg.adaptive_lookahead ? "adaptive" : "off",
                cfg.lookahead_depth);
        printf ("cache   : %s (%s)\n", cfg.use_cache ? "on" : "off", cache_dir);
      }
  }
  if (check_only)
    {
      printf ("moosedog: %s OK (%zu rules, %zu tokens)\n", grm_path,
              spec.rule_count, spec.token_count);
      rc = 0;
      goto done;
    }
  {
    char base[256];
    memset (&eopt, 0, sizeof (eopt));
    if (stem)
      snprintf (base, sizeof (base), "%s", stem);
    else
      md_emit_default_basename (&spec, grm_path, base, sizeof (base));
    /* Persist basename for emission. */
    {
      static char basebuf[256];
      snprintf (basebuf, sizeof (basebuf), "%s", base);
      eopt.stem = basebuf;
      eopt.output_dir = output_dir;
      eopt.emit_makefile = emit_makefile;
      eopt.emit_cmakefile = emit_cmakefile;
      eopt.debug = debug;
      eopt.no_rewrite = no_rewrite;
      eopt.max_errors = (int) max_errors;
      eopt.use_cache = use_cache;
      eopt.cache_dir = NULL;
      eopt.strict_unicode = strict_unicode;
      eopt.moosedog_include = moosedog_include;
      if (md_emit_all (&spec, &cfg, &eopt, &err) != 0)
        {
          fprintf (stderr, "moosedog: %s\n", err.message);
          goto done;
        }
      printf ("moosedog: wrote %s/%s.{ast.h,lexer.h,parser.h,parser.c}\n",
              output_dir, base);
      rc = 0;
    }
  }
done:
  if (preprocessed)
    {
      unlink (preprocessed);
      free (preprocessed);
    }
  md_spec_destroy (&spec);
  md_plugins_destroy (&preg);
  return rc;
}
